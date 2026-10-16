/**
 * OMTChannel.cpp - Client Connection Channel Implementation
 * Port of libomtnet/src/OMTChannel.cs
 */

#include "OMTChannel.h"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/tcp.h>
// <sys/select.h> is gone with the select() call: receiverLoop() uses poll() now (see the comment
// there), and leaving the header in place invited the FD_SETSIZE bug back.

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "omt-channel"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) 
#define LOGE(...)
#define LOGD(...)
#endif

namespace omt {

namespace {
/// The ceiling for a peer's control frame. metadataLength is a uint16 on the wire, so no
/// conforming frame can describe a payload bigger than this; anything above it is either hostile
/// or a frame type this direction does not accept. The receive buffer is sized from the same
/// constant, so an accepted frame can never fail to fit.
constexpr int32_t kMaxControlPayload = 65535;

/// The kernel naming the connection dead is the ONLY positive evidence that a peer is gone.
/// Everything else — above all a peer that simply stopped reading, or a read that was interrupted
/// or found nothing to take — is back-pressure or a race, and must leave the channel alone.
///
/// A peer that closed only its WRITE half (a pure receiver that shutdown(SHUT_WR)s after the
/// subscribe XML, which is what OBS, libomtnet and our own desktop receiver do) is deliberately NOT
/// in this list: that state is invisible to the read side and irrelevant to us. It becomes a death
/// only if a later send errors, which is the difference between "the viewer hung up" and "the viewer
/// is a listener with nothing to say".
///
/// Ported from the desktop vendored copy (nodos plugins/yd.net.ompout/vendor/libomtcpp/src/
/// OMTChannel.cpp), which has carried this list since 2026-10-13; this side never received it.
bool IsPeerGoneErrno(int e) {
    switch (e) {
        case EPIPE:         // peer closed its side; our writes now fail
        case ECONNRESET:    // peer (or a middlebox) sent RST
        case ENOTCONN:      // the connection is already gone
        case ECONNABORTED:  // locally aborted connection
        case ESHUTDOWN:     // send after the connection was shut down
        case EHOSTUNREACH:
        case ENETUNREACH:
        case ENETDOWN:
            return true;
        default:
            return false;    // EAGAIN/ENOMEM/EINVAL/...: transient, never a death sentence
    }
}

/// Consecutive transient (non-fatal) read results we tolerate with no progress in sight. The
/// receive path only reaches this counter when select() reported the socket readable and recv() then
/// refused the data, so a streak this long is not a slow peer — it is a socket that will never
/// deliver, and looping on it would pin a core at 100% CPU for the life of the app.
constexpr int kMaxConsecutiveTransientReads = 100;
} // namespace

Channel::Channel(int socketFd, struct sockaddr_in address, 
                 int sendBufferSize, IChannelListener* listener)
    : socketFd_(socketFd), address_(address), listener_(listener) {
    
    // Set non-blocking for recv
    int flags = fcntl(socketFd_, F_GETFL, 0);
    fcntl(socketFd_, F_SETFL, flags | O_NONBLOCK);
    
    // TCP_NODELAY for low latency
    int one = 1;
    setsockopt(socketFd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    
    // Set send buffer
    setSendBuffer(sendBufferSize);
    
    // TCP keepalive
    setsockopt(socketFd_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    
    // TCP_KEEPIDLE = 5 seconds (matches official libomtnet) — but idle alone is not a timeout.
    // With KEEPINTVL/KEEPCNT left at their kernel defaults (75s between probes, 9 probes) a peer
    // that vanished without sending RST takes ~11 minutes to be declared dead, and every frame in
    // the meantime is written into a black hole. Probing hard makes that ~11s.
#ifdef TCP_KEEPIDLE
    int keepIdle = Constants::NETWORK_KEEPALIVE_IDLE_S;
    setsockopt(socketFd_, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(keepIdle));
#endif
#ifdef TCP_KEEPINTVL
    int keepIntvl = Constants::NETWORK_KEEPALIVE_INTVL_S;
    setsockopt(socketFd_, IPPROTO_TCP, TCP_KEEPINTVL, &keepIntvl, sizeof(keepIntvl));
#endif
#ifdef TCP_KEEPCNT
    int keepCnt = Constants::NETWORK_KEEPALIVE_CNT;
    setsockopt(socketFd_, IPPROTO_TCP, TCP_KEEPCNT, &keepCnt, sizeof(keepCnt));
#endif
    
    // Create async send pool
    sendPool_ = std::make_unique<AsyncPool>(
        Constants::NETWORK_ASYNC_COUNT, 
        Constants::NETWORK_ASYNC_BUFFER_AV);
    
    // Start threads
    // Start threads
    senderThread_ = std::thread(&Channel::senderLoop, this);
    receiverThread_ = std::thread(&Channel::receiverLoop, this);
    
    // Initial metadata handshake is now handled by Sender
    
    LOGI("Channel: connected %s:%d", getRemoteAddress().c_str(), getRemotePort());
}

Channel::~Channel() {
    LOGI("Channel: destroying");
    running_ = false;
    connected_ = false;
    
    // Shutdown socket to unblock threads
    if (socketFd_ > 0) {
        shutdown(socketFd_, SHUT_RDWR);
    }
    
    queueCV_.notify_all();
    
    if (receiverThread_.joinable()) {
        receiverThread_.join();
    }
    if (senderThread_.joinable()) {
        senderThread_.join();
    }
    
    // Clear pending queue
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        while (!pendingQueue_.empty()) {
            sendPool_->release(pendingQueue_.front());
            pendingQueue_.pop();
        }
    }
    
    closeSocket();
    
    LOGI("Channel: destroyed, sent=%lld, dropped=%lld", 
         (long long)framesSent_.load(), (long long)framesDropped_.load());
}

void Channel::disconnect() {
    connected_ = false;
    running_ = false;
    closeSocket();
}

void Channel::closeSocket() {
    if (socketFd_ > 0) {
        close(socketFd_);
        socketFd_ = -1;
    }
}

void Channel::setSendBuffer(int size) {
    setsockopt(socketFd_, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    
    int actualBuf = 0;
    socklen_t optlen = sizeof(actualBuf);
    getsockopt(socketFd_, SOL_SOCKET, SO_SNDBUF, &actualBuf, &optlen);
    LOGI("Channel: SO_SNDBUF=%d (requested %d)", actualBuf, size);
}

std::string Channel::getRemoteAddress() const {
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &address_.sin_addr, buf, sizeof(buf));
    return std::string(buf);
}

int Channel::getRemotePort() const {
    return ntohs(address_.sin_port);
}

ChannelStats Channel::getStats() const {
    return {
        framesSent_.load(),
        framesDropped_.load(),
        bytesSent_.load(),
        bytesReceived_.load()
    };
}

bool Channel::sendMetadataSync(const char* xml) {
    if (!connected_) return false;
    
    int len = strlen(xml);
    int totalLen = len + 1;
    
    FrameHeader header{};
    header.version = 1;
    header.frameType = static_cast<uint8_t>(FrameType::Metadata);
    header.timestamp = 0;
    header.metadataLength = static_cast<uint16_t>(totalLen);
    header.dataLength = totalLen;
    
    // Was: clear O_NONBLOCK, fire two bare send()s, restore the flags. A wedged subscriber made
    // this block indefinitely, and its only caller (Sender::sendMetadata) held channelsMutex_ —
    // the same mutex the camera thread takes to fan out frames — so ONE stalled viewer stopped
    // the whole stream, not just that viewer's. One buffer, one deadline, no flag changes.
    std::vector<char> buf(sizeof(FrameHeader) + (size_t)totalLen);
    memcpy(buf.data(), &header, sizeof(FrameHeader));
    memcpy(buf.data() + sizeof(FrameHeader), xml, len);
    buf[sizeof(FrameHeader) + len] = 0;

    // P0 #7 (ported from the desktop vendored copy): take the per-channel write lock for the WHOLE
    // frame. senderLoop is writing video to the same fd, and a metadata frame whose bytes land in
    // the middle of a half-written video record does not lose a frame — it shifts the subscriber's
    // parse offset for the rest of the connection.
    //
    // Acquisition is bounded, because the holder of that lock may be a writer that is itself
    // waiting on a stalled peer for up to NETWORK_SEND_TIMEOUT_MS. try_lock() polled against a
    // budget (std::mutex, not std::timed_mutex — see the member's comment for why).
    const auto budget = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(Constants::NETWORK_METADATA_LOCK_BUDGET_MS);
    while (!sendSocketMutex_.try_lock()) {
        if (!running_ || !connected_) return false;
        if (std::chrono::steady_clock::now() >= budget) {
            LOGE("sendMetadataSync: could not acquire the socket-write lock within %d ms, "
                 "dropping this subscriber", Constants::NETWORK_METADATA_LOCK_BUDGET_MS);
            // Marking the channel dead is the only safe response: the alternative is to keep the
            // caller waiting behind a writer that has already used its whole budget.
            connected_ = false;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::lock_guard<std::mutex> writeLock(sendSocketMutex_, std::adopt_lock);

    const int fd = socketFd_;          // read once, under the write lock
    if (fd < 0) { connected_ = false; return false; }

    bool ok = sendWithDeadline(fd, reinterpret_cast<const uint8_t*>(buf.data()), buf.size(),
                               Constants::NETWORK_SEND_TIMEOUT_MS);
    if (!ok) {
        LOGE("sendMetadataSync: incomplete metadata write, dropping this subscriber");
        // Still holding sendSocketMutex_: a PARTIAL write leaves half a metadata record on the
        // stream, so the channel must be dead — and visibly dead — before any waiting video writer
        // can acquire the lock and append a full record behind the tear.
        noteCloseReason(CloseReason::SendFailed);
        connected_ = false;
    }
    return ok;
}

int Channel::sendAsync(const void* headerData, size_t headerLen,
                       const void* extHeaderData, size_t extHeaderLen,
                       const void* payloadData, size_t payloadLen) {
    if (!connected_) return 0;
    
    size_t totalLen = headerLen + extHeaderLen + payloadLen;
    
    if (totalLen > static_cast<size_t>(Constants::VIDEO_MAX_SIZE)) {
        framesDropped_++;
        LOGE("sendAsync: frame too large %zu", totalLen);
        return 0;
    }
    
    AsyncBuffer* buf = sendPool_->acquire();
    if (!buf) {
        // Was: drop THIS frame and keep whatever is queued. With 4 slots and a subscriber that
        // stalled for a moment, the queue fills with the oldest frames; when the window reopens
        // the senderLoop replays up to 4 already-decoded frames (133 ms at 30fps) before anything
        // live reaches the screen — the stall shows up as latency instead of as a few dropped
        // frames. For a live stream the newest frame is the only one worth sending, so evict the
        // oldest queued buffer, hand its slot to this frame, and count the eviction.
        AsyncBuffer* stale = nullptr;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (!pendingQueue_.empty()) { stale = pendingQueue_.front(); pendingQueue_.pop(); }
        }
        if (stale) {
            sendPool_->release(stale);
            staleEvicted_++;
            framesDropped_++;
            buf = sendPool_->acquire();
        }
    }
    if (!buf) {
        framesDropped_++;
        static int dropLog = 0;
        if (dropLog++ % 30 == 0) {
            LOGD("sendAsync: pool exhausted (dropped: %lld)",
                 (long long)framesDropped_.load());
        }
        return 0;
    }
    
    buf->resize(totalLen);
    
    size_t offset = 0;
    memcpy(buf->data.data() + offset, headerData, headerLen);
    offset += headerLen;
    memcpy(buf->data.data() + offset, extHeaderData, extHeaderLen);
    offset += extHeaderLen;
    memcpy(buf->data.data() + offset, payloadData, payloadLen);
    buf->length = totalLen;
    
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        pendingQueue_.push(buf);
    }
    queueCV_.notify_one();
    
    return static_cast<int>(totalLen);
}

void Channel::senderLoop() {
    LOGI("senderLoop: started");
    
    while (running_ && connected_) {
        AsyncBuffer* buf = nullptr;
        
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCV_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                return !pendingQueue_.empty() || !running_ || !connected_;
            });
            
            if (!running_ || !connected_) break;
            if (pendingQueue_.empty()) continue;
            
            buf = pendingQueue_.front();
            pendingQueue_.pop();
        }
        
        if (!buf) continue;
        
        bool success = sendAllBlocking(buf->data.data(), buf->length);
        
        if (success) {
            framesSent_++;
            bytesSent_ += buf->length;
        } else {
            // Neither counted before: the frame left the queue, wrote some bytes, and appeared in
            // no counter at all — so framesSent + framesDropped did not add up to frames offered,
            // and the adaptive-quality logic under-reacted exactly when the link was failing.
            framesDropped_++;
            tornSends_++;
        }
        
        sendPool_->release(buf);
    }
    
    LOGI("senderLoop: exiting");
}

// Write the whole buffer, but never longer than the deadline. The socket stays non-blocking the
// whole time (as Channel's constructor set it up) and poll() decides when a send is worth trying,
// which is why there is no fcntl() flag toggling here any more: toggling a shared fd to blocking
// mode meant any other thread that sent on it during that window — or any early return that
// missed the restore — inherited a socket whose behaviour had silently changed.
bool Channel::sendWithDeadline(int fd, const uint8_t* data, size_t length, int timeoutMs) {
    // Callers hold sendSocketMutex_ (see OMTChannel.h): this function is the one and only way
    // anything writes to the subscriber, and it is only ever entered under that lock.
    size_t remaining = length;
    const uint8_t* ptr = data;
    // P0 #7: timeoutMs (Constants::NETWORK_SEND_TIMEOUT_MS, unchanged at 1000 ms so the eviction
    // timing the slow-subscriber gate already pins does not move) bounds the ENTIRE write. Bounding
    // only each poll() is what let a peer that
    // drains a few bytes per second hold a 1.5 MB frame open indefinitely — poll keeps succeeding,
    // the per-poll deadline never fires, and the sender thread never comes back for the next frame.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (remaining > 0) {
        // P0 #7: and the write stops when the channel stops being alive. ~Channel() and disconnect()
        // clear these; without the check a bounded-but-still-running write could outlive the
        // teardown that is supposed to interrupt it.
        if (!running_ || !connected_) {
            LOGE("sendWithDeadline: channel is gone, abandoning the write at %zu/%zu bytes",
                 length - remaining, length);
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            LOGE("sendWithDeadline: peer did not take the whole buffer in %d ms — evicting "
                 "(%zu/%zu bytes written)", timeoutMs, length - remaining, length);
            return false;
        }
        // A poll slice, not the deadline: the slice is how often the two checks above are re-run,
        // the deadline is how long the whole write may take. Bounding only the slice is what left a
        // trickle peer able to hold the frame open forever.
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        const int sliceMs = (int)(left < Constants::NETWORK_SEND_POLL_SLICE_MS
                                      ? (left < 1 ? 1 : left)
                                      : Constants::NETWORK_SEND_POLL_SLICE_MS);

        struct pollfd pfd;
        pfd.fd = fd;                       // the caller's snapshot, not the member re-read
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, sliceMs);
        if (pr == 0) continue;            // slice expired — re-check alive + overall deadline
        if (pr < 0) {
            if (errno == EINTR) continue;
            LOGE("sendWithDeadline: poll error %d (%s)", errno, strerror(errno));
            return false;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;

        ssize_t sent = send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (sent > 0) { ptr += (size_t)sent; remaining -= (size_t)sent; continue; }
        if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        LOGE("sendWithDeadline: send error %d (%s)", errno, strerror(errno));
        return false;
    }
    return true;
}

bool Channel::sendAllBlocking(const uint8_t* data, size_t length) {
    // Name kept for the call sites; it no longer blocks without bound.
    //
    // P0 #7: the same per-channel write lock sendMetadataSync takes, held for the whole frame, so
    // the async video writer and the sync metadata writer cannot interleave on one TCP stream. The
    // fd is read inside the lock — closeSocket() clears it, and a write that began with a stale
    // snapshot would be sending on a descriptor the kernel may already have handed to another
    // socket.
    std::lock_guard<std::mutex> writeLock(sendSocketMutex_);
    const int fd = socketFd_;
    if (fd < 0) return false;
    bool ok = sendWithDeadline(fd, data, length, Constants::NETWORK_SEND_TIMEOUT_MS);
    // Under the lock, deliberately: whoever is waiting to write next must never get a turn in the
    // gap between a torn frame and the channel being marked dead, or it appends a clean record
    // behind the tear and the subscriber is mis-framed for the rest of the connection.
    if (!ok) {
        noteCloseReason(CloseReason::SendFailed);
        connected_ = false;
    }
    return ok;
}

void Channel::receiverLoop() {
    LOGI("receiverLoop: started");

    // Sized so that the largest frame the validation below can ACCEPT always fits in one buffer:
    // 16 + kMaxControlPayload. That is what makes the "buffer is full" branch reachable only for a
    // peer streaming bytes we have already refused to parse — with the old flat 65536 buffer a
    // legitimate 65535-byte control frame could never be assembled, and the peer would be evicted
    // for sending something conforming.
    std::vector<uint8_t> recvBuffer(FrameHeader::SIZE + static_cast<size_t>(kMaxControlPayload));
    size_t bufferPos = 0;
    // Consecutive transient read results, reset by any byte actually taken. See
    // kMaxConsecutiveTransientReads for why a streak, however long it stays alive, must end.
    int transientStreak = 0;

    while (running_ && connected_) {
        // socketFd_ is cleared by closeSocket()/disconnect() on another thread. FD_SET(-1) and
        // poll(-1) are both undefined, so snapshot it once per iteration and bail if it is gone.
        const int fd = socketFd_;
        if (fd < 0) break;

        // poll(), not select(). FD_SET() writes a bit into a STACK-resident fd_set sized for
        // FD_SETSIZE (1024) descriptors, so for any accepted socket with fd >= 1024 — routine on a
        // phone that has been up for a day, and routine in the host QA process — it wrote past the
        // end of that array; ASan reports it as a stack-buffer-overflow inside receiverLoop. select()
        // then refuses the call anyway (nfds > FD_SETSIZE). poll() has no descriptor ceiling.
        struct pollfd pfd{fd, POLLIN, 0};
        int sel = ::poll(&pfd, 1, 100);  // 100ms tick: running_ stays observable

        if (sel < 0) {
            if (errno == EINTR) continue;
            LOGE("receiverLoop: poll error %d", errno);
            noteCloseReason(CloseReason::PollError);
            connected_ = false;
            break;
        }

        if (sel == 0) continue;

        // Never call recv() with zero capacity: a full buffer means a peer is streaming a frame
        // larger than kMaxControlPayload (already rejected at parse time) or we failed to drain.
        if (bufferPos >= recvBuffer.size()) {
            LOGE("receiverLoop: receive buffer full (%zu) without a consumable frame", bufferPos);
            noteCloseReason(CloseReason::RecvBufferFull);
            connected_ = false;
            break;
        }

        ssize_t received = recv(fd, recvBuffer.data() + bufferPos,
                                recvBuffer.size() - bufferPos, 0);

        // ---- what a failed or empty read means (ported from the desktop vendored copy) --------
        // The whole loop used to be `if (received <= 0) { …; connected_ = false; break; }`, and the
        // errno test below it only decided which line to LOG, not whether to die. Two perfectly
        // healthy peers were killed by it, both measured against this exact file (qa-ompin/
        // FOREIGN-IN-DIAGNOSTICS-20261015.md, scenarios D1 and D4):
        //
        //   * D1 — a peer that shutdown(SHUT_WR)s after subscribing. That is what a pure receiver
        //     is: OBS, libomtnet, and our own OmpIn all close the half they never intend to write
        //     from. recv() then answers 0 forever, the channel is marked dead, and the subscriber
        //     gets the 86-byte greeting and no video at all — measured 86 bytes / 0 frames against
        //     3.9 MB / 179 frames for the identical peer with no half-close. The only difference
        //     between the two runs was the half-close.
        //   * D4 — one spurious readability: select() said readable, and a competing reader (or an
        //     SO_RCVLOWAT race) took the bytes before recv() ran. recv() answers EAGAIN, the channel
        //     dies, and the SUBSCRIBE ITSELF never registers — 86 bytes, then PEER_CLOSED. Measured
        //     239/239 frames once this branch was applied as a control.
        //
        // A dead channel is also silent, which is why this survived an audit cycle: only
        // ~Channel() closes the descriptor, and that runs from cleanupDisconnected(), which is only
        // reached from Sender::send()/getStats(). A peer that is not getting frames has no way to
        // say so, and the receiver side just sits in CONNECTING with an empty LastError.
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            // Nothing to read right now. NOT a disconnect — keep the channel and poll again.
            //
            // Logged once per episode, not per attempt: the desktop copy learned this the hard way
            // when the per-attempt form printed 802 identical warnings in three minutes against one
            // slow OBS. The flag is re-armed by the next read that actually returns bytes (below),
            // so a genuinely NEW stall is still reported.
            if (!warnedReadTransient_.exchange(true, std::memory_order_relaxed)) {
                readTransientWarnings_++;
                LOGE("receiverLoop: %d consecutive transient reads (last errno %d, %s) — peer kept",
                     transientStreak + 1, errno, strerror(errno));
            }
            if (++transientStreak > kMaxConsecutiveTransientReads) {
                LOGE("receiverLoop: %d transient reads in a row with no byte taken — giving up on "
                     "this socket", transientStreak);
                connected_ = false;
                break;
            }
            continue;
        }
        if (received < 0) {
            if (IsPeerGoneErrno(errno)) {
                LOGE("receiverLoop: peer gone (%d, %s)", errno, strerror(errno));
                noteCloseReason(CloseReason::PeerClosed);
            } else {
                // An errno the kernel does not use to say "the peer is gone". The read path cannot
                // make progress on it either, so it ends the channel — but it is named, because
                // "a peer hung up" and "the read path broke" must not be the same observable state.
                LOGE("receiverLoop: recv error %d (%s) — not a peer-gone errno", errno, strerror(errno));
                noteCloseReason(CloseReason::RecvError);
            }
            connected_ = false;
            break;
        }
        if (received == 0) {
            // Read-half EOF is a STATE, not an event to keep polling for: the socket stays readable
            // forever, so the only correct response is to stop reading (break, do not continue) while
            // leaving the channel ALIVE. The send side is a different half of the connection and
            // senderLoop must keep running — this whole file exists to push video at a peer that has
            // nothing left to say.
            //
            // A full close still gets reaped: it surfaces later as a send error (sendWithDeadline
            // fails on POLLHUP/POLLERR or on the kernel's own errno), which clears connected_ under
            // the write lock, and cleanupDisconnected() then joins and closes the channel.
            LOGI("receiverLoop: peer write-half closed (EOF); stopping reads, keep streaming");
            break;
        }

        // Bytes really moved: re-arm the once-per-episode warning and clear the transient streak.
        transientStreak = 0;
        warnedReadTransient_.store(false, std::memory_order_relaxed);

        bytesReceived_ += received;
        bufferPos += received;

        // Process complete frames
        while (bufferPos >= FrameHeader::SIZE) {
            // Re-check the flags EVERY iteration, inside the parse loop: ~Channel() joins this
            // thread, and a peer that pipelines a batch of frames gets to decide how long that
            // join takes otherwise.
            if (!running_ || !connected_) break;

            // Copy the header into a local BEFORE reading any field. recvBuffer.data() is
            // attacker-controlled; the old reinterpret_cast<FrameHeader*> dereferenced it in place
            // and, worse, fed its fields into arithmetic before anything checked them.
            FrameHeader header;
            std::memcpy(&header, recvBuffer.data(), FrameHeader::SIZE);

            // Validate every field first. dataLength is a SIGNED int32 off the wire and the old
            // `size_t totalFrameLen = FrameHeader::SIZE + header->dataLength` made dataLength = -16
            // come out as totalFrameLen = 0: the shift at the bottom of this loop then consumed ZERO
            // bytes, so the same 16 bytes were re-parsed forever — a permanent 100% CPU spin on a
            // single TCP connect to port 6400, with no flag re-checked and no way out. ~Channel()
            // joins this thread, so the app could not even tear the stream down: the only recovery
            // was killing the process.
            if (header.version != 1) {
                LOGE("receiverLoop: invalid version %u", header.version);
                noteCloseReason(CloseReason::BadVersion);
                connected_ = false;
                break;
            }
            // This is the SENDER's receive path: a peer may only send Metadata control frames
            // (subscribe / tally / quality XML). Video or Audio here is malformed for this direction.
            if (header.frameType != static_cast<uint8_t>(FrameType::Metadata)) {
                LOGE("receiverLoop: unexpected peer frameType %u", header.frameType);
                noteCloseReason(CloseReason::BadFrameType);
                connected_ = false;
                break;
            }
            if (header.dataLength < 0) {
                LOGE("receiverLoop: negative dataLength %d", header.dataLength);
                noteCloseReason(CloseReason::NegativeDataLength);
                connected_ = false;
                break;
            }
            if (header.dataLength > kMaxControlPayload) {
                LOGE("receiverLoop: oversized control frame dataLength %d", header.dataLength);
                noteCloseReason(CloseReason::OversizedDataLength);
                connected_ = false;
                break;
            }
            // metadataLength is uint16 and must fit inside the payload it claims to describe.
            if (static_cast<int32_t>(header.metadataLength) > header.dataLength) {
                LOGE("receiverLoop: metadataLength %u > dataLength %d",
                     header.metadataLength, header.dataLength);
                noteCloseReason(CloseReason::MetadataLenTooBig);
                connected_ = false;
                break;
            }

            // Both operands are non-negative and bounded here, so this cannot wrap or go negative.
            const size_t totalFrameLen =
                static_cast<size_t>(FrameHeader::SIZE) + static_cast<size_t>(header.dataLength);

            if (totalFrameLen > bufferPos) break;  // wait for the rest of the frame

            // Process metadata
            if (header.dataLength > 0) {
                char* xmlData = reinterpret_cast<char*>(recvBuffer.data() + FrameHeader::SIZE);

                // Fix for conflicting implementations:
                // - C# libomtnet sends XML *without* null terminator
                // - Protocol says null terminated
                // - C++ sender sends *with* null terminator
                size_t strLen = header.dataLength;
                if (strLen > 0 && xmlData[strLen - 1] == '\0') {
                    strLen--;
                }

                std::string xml(xmlData, strLen);
                processMetadata(xml);
            }

            // Shift buffer. Every frame that reaches this point has dataLength >= 0, so
            // totalFrameLen >= FrameHeader::SIZE and bufferPos strictly decreases by at least 16 —
            // the zero-progress loop the old code could enter is not reachable from here.
            if (totalFrameLen < bufferPos) {
                memmove(recvBuffer.data(), recvBuffer.data() + totalFrameLen,
                        bufferPos - totalFrameLen);
            }
            bufferPos -= totalFrameLen;
        }
    }

    LOGI("receiverLoop: exiting");
    
    // Notify listener
    if (listener_ && !connected_) {
        listener_->onDisconnected(this);
    }
}

void Channel::processMetadata(const std::string& xml) {
    // Subscription commands
    if (xml == MetadataConstants::CHANNEL_SUBSCRIBE_VIDEO) {
        subscriptions_ = subscriptions_.load() | Subscription::Video;
        LOGI("Client subscribed to VIDEO");
        return;
    }
    if (xml == MetadataConstants::CHANNEL_SUBSCRIBE_AUDIO) {
        subscriptions_ = subscriptions_.load() | Subscription::Audio;
        LOGI("Client subscribed to AUDIO");
        return;
    }
    if (xml == MetadataConstants::CHANNEL_SUBSCRIBE_METADATA) {
        subscriptions_ = subscriptions_.load() | Subscription::Metadata;
        LOGI("Client subscribed to METADATA");
        return;
    }
    
    // Tally commands
    if (xml == MetadataConstants::TALLY_PREVIEWPROGRAM) {
        tallyPreview_ = true; tallyProgram_ = true;
        if (listener_) listener_->onTallyChanged(this);
        return;
    }
    if (xml == MetadataConstants::TALLY_PROGRAM) {
        tallyPreview_ = false; tallyProgram_ = true;
        if (listener_) listener_->onTallyChanged(this);
        return;
    }
    if (xml == MetadataConstants::TALLY_PREVIEW) {
        tallyPreview_ = true; tallyProgram_ = false;
        if (listener_) listener_->onTallyChanged(this);
        return;
    }
    if (xml == MetadataConstants::TALLY_NONE) {
        tallyPreview_ = false; tallyProgram_ = false;
        if (listener_) listener_->onTallyChanged(this);
        return;
    }
    
    // Preview mode
    if (xml == MetadataConstants::CHANNEL_PREVIEW_VIDEO_ON) {
        previewMode_ = true;
        return;
    }
    if (xml == MetadataConstants::CHANNEL_PREVIEW_VIDEO_OFF) {
        previewMode_ = false;
        return;
    }
    
    // Quality suggestion
    if (xml.find(MetadataConstants::SUGGESTED_QUALITY_PREFIX) == 0) {
        Quality oldQuality = suggestedQuality_.load();
        
        if (xml.find("\"Low\"") != std::string::npos) {
            suggestedQuality_ = Quality::Low;
        } else if (xml.find("\"Medium\"") != std::string::npos) {
            suggestedQuality_ = Quality::Medium;
        } else if (xml.find("\"High\"") != std::string::npos) {
            suggestedQuality_ = Quality::High;
        } else {
            suggestedQuality_ = Quality::Default;
        }
        
        if (suggestedQuality_.load() != oldQuality && listener_) {
            listener_->onQualityChanged(this);
        }
        return;
    }
    
    LOGD("Unknown metadata: %s", xml.c_str());
}

} // namespace omt
