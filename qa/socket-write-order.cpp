/*
 * socket-write-order.cpp — proves one subscriber's socket has exactly one writer at a time, that
 * the write is bounded as a whole, and that a failed write takes the channel down before anyone
 * else can put bytes on the stream.
 * (P0 #7, ported from the desktop vendored copy at
 *  nodos-yded-linux/plugins/yd.net.ompout/vendor/libomtcpp/src/OMTChannel.cpp — sendAllPoll and
 *  the sendSocketMutex_ in sendMetadataSync/sendAllBlocking.)
 *
 * Why this exists: Channel declared queueMutex_ and nothing else. sendMetadataSync runs on the
 * accept-loop thread at handshake and on the JNI/UI thread via Sender::sendMetadata; senderLoop
 * runs on its own thread. Both write the same fd, with no lock between them, and each write is a
 * loop of partial send() calls — so a metadata broadcast that overlaps a video frame splices the
 * metadata bytes into the middle of the video record. TCP has no message boundaries: the
 * subscriber's parser is then at the wrong offset for every record that follows. The stream does
 * not "lose a frame", it is mis-framed until the connection closes, and nothing on the sender side
 * ever notices, because both writers succeeded.
 *
 * The second half is the bound. sendWithDeadline() bounded each poll(), not the write: a peer that
 * drains a few hundred bytes every second keeps poll() succeeding forever, so one trickle
 * subscriber can hold a 1.5 MB frame in flight indefinitely — the sender thread never returns to
 * the queue, the AsyncPool slot never comes back, and the deadline that was supposed to evict it
 * never fires. It also had no alive check, so ~Channel could not interrupt it, and it cleared
 * connected_ after releasing nothing, so a writer waiting on the fd could append bytes behind the
 * torn frame.
 *
 * Build + run on the host (no device, no JVM):  bash qa/build-host.sh
 * Expected: ALL PASS on the fixed code. With the fix reverted on a copy of the tree the same binary
 * must go red on the record-integrity checks (S1, S4), on the eviction deadline (S2) and on the
 * in-flight-write teardown (S3). That A/B is what proves the test has teeth.
 *
 * S4 exists because S1 alone did not. S1 releases both writers and hopes they collide; when the
 * kernel happened to let the video writer drain each record before the metadata writer got a slice
 * of window, the sw-mutant-video-unlocked build produced a perfectly clean wire and stayed GREEN —
 * a gate whose red depends on thread scheduling is not a gate. S4 removes the hope: the peer reads
 * nothing at all until the scenario has proved from outside that the video writer is parked
 * mid-record (POLLOUT refused, FIONREAD+TIOCOUTQ frozen), then the second writer is released into
 * the rounds of window the peer opens one recv() at a time and the two compete for every one of
 * them. The assertion is on byte-level record integrity and on the counters snapshotted the instant
 * the metadata call returns — never on a wall-clock window.
 *
 * One trap worth recording, because it is how this file first fooled itself: ~Channel() calls
 * shutdown(SHUT_RDWR) before it joins the sender thread, and that alone breaks a blocked poll(). A
 * scenario that marks the channel dead BY DESTROYING THE CHANNEL therefore passes with the alive
 * check deleted, and proves nothing. S3 kills the channel with the peer's FIN instead — connected_
 * goes false with the descriptor untouched — and qa/build-host.sh's sw-mutant-no-alive-check is the
 * evidence that only that staging separates fixed from broken.
 */
#include "OMTChannel.h"
#include "OMTConstants.h"
#include "OMTProtocol.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace omt;
using clock_ = std::chrono::steady_clock;

namespace {

int failures = 0;

void ok(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++failures;
}

long msSince(clock_::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_::now() - t0).count();
}

// --------------------------------------------------------------- reading the socket from outside --
// Three probes the scenarios use to prove a write is stuck rather than merely slow. They are read
// from the test, on the two descriptors it owns, so nothing has to be added to the production
// class for the gate to have an observable.
//
//   socketCanAccept  — poll(POLLOUT, 0). False means the send buffer is full: the writer inside
//                      sendWithDeadline cannot be making progress, it is parked in poll().
//   queuedForReader  — FIONREAD on the peer's end: bytes the sender completed and the kernel is
//                      holding for the application.
//   queuedForSender  — TIOCOUTQ on the sender's end: bytes transmitted and not yet acknowledged.
//                      The two together are the bytes the blocked writer has actually managed to
//                      put on the wire, which is the "partial fill" the block assertion needs —
//                      they are read WITHOUT the peer having to consume anything.
bool socketCanAccept(int fd) {
    struct pollfd p{};
    p.fd = fd;
    p.events = POLLOUT;
    p.revents = 0;
    int r = ::poll(&p, 1, 0);
    return r > 0 && !(p.revents & (POLLERR | POLLHUP | POLLNVAL));
}
long queuedForReader(int fd) {
    int n = -1;
    if (fd < 0 || ::ioctl(fd, FIONREAD, &n) != 0) return -1;
    return n;
}
long queuedForSender(int fd) {
    int n = -1;
    if (fd < 0 || ::ioctl(fd, TIOCOUTQ, &n) != 0) return -1;
    return n;
}

// ------------------------------------------------------------------ the two subscriber shapes ---
// A peer that stops draining and then drains everything (S1), one that drains a trickle
// forever (S2/S3), and one that reads nothing at all until the scenario tells it to and then
// opens the window in measured rounds (S4). All keep every byte they receive so the scenario
// can parse the wire.
struct Peer {
    enum Mode { DRAIN_AFTER_STALL, TRICKLE, GATED_DRAIN };
    int listenFd = -1;
    int accepted = -1;
    unsigned short port = 0;
    Mode mode = DRAIN_AFTER_STALL;
    long stallMs = 700;              // DRAIN_AFTER_STALL: how long to read nothing at all
    int trickleBytes = 512;          // TRICKLE: how much to take per interval
    int trickleEveryMs = 250;
    int rcvBuf = 8192;               // 0 = leave the kernel default (a peer that drains fast)
    int drainChunk = 32768;          // GATED_DRAIN: bytes released to the window per round
    int drainEveryMs = 10;           // GATED_DRAIN: pause between rounds
    int senderFd = -1;               // GATED_DRAIN: the other end, so the peer can see the window
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> drainGo{false};      // GATED_DRAIN: leave phase 1
    std::atomic<bool> stopNow{false};      // any mode: break phase 1's wait so stop() cannot hang
    std::atomic<bool> writersGo{false};    // GATED_DRAIN: the window is open — release writer #2
    std::atomic<int> drainRounds{0};       // GATED_DRAIN: how many collision rounds actually ran
    std::vector<uint8_t> rx;
    std::mutex rxMutex;
    std::thread th;

    void start() {
        listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        bind(listenFd, (sockaddr*)&a, sizeof(a));
        socklen_t sl = sizeof(a);
        getsockname(listenFd, (sockaddr*)&a, &sl);
        port = ntohs(a.sin_port);
        listen(listenFd, 4);
        th = std::thread([this] {
            accepted = ::accept(listenFd, nullptr, nullptr);
            if (accepted < 0) { finished = true; return; }
            if (rcvBuf > 0) {   // close the advertised window after a couple of frames
                setsockopt(accepted, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(int));
            }
            started = true;
            if (mode == DRAIN_AFTER_STALL) {
                std::this_thread::sleep_for(std::chrono::milliseconds(stallMs));
                // ...then read everything the sender has, until it stops.
                for (;;) {
                    uint8_t buf[65536];
                    ssize_t n = ::recv(accepted, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    std::lock_guard<std::mutex> lk(rxMutex);
                    rx.insert(rx.end(), buf, buf + n);
                }
            } else if (mode == TRICKLE) {
                for (;;) {
                    std::vector<uint8_t> buf((size_t)trickleBytes);
                    ssize_t n = ::recv(accepted, buf.data(), buf.size(), 0);
                    if (n <= 0) break;      // closed by the sender, or reset by shutdown()
                    {
                        std::lock_guard<std::mutex> lk(rxMutex);
                        rx.insert(rx.end(), buf.data(), buf.data() + n);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(trickleEveryMs));
                }
            } else {
                // GATED_DRAIN, phase 1 — read NOTHING. The video writer fills the peer's receive
                // buffer and its own send buffer and parks in poll(); the scenario proves that from
                // outside (FIONREAD + TIOCOUTQ frozen, POLLOUT refused) before anybody else is
                // allowed near the descriptor.
                while (!drainGo && !stopNow)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                // Phase 2 — open the window in measured rounds. Each recv() releases drainChunk of
                // advertised window; the sender's stack wakes every writer parked on that socket,
                // so each round is one independent chance for whichever writer is unserialised to
                // cut into the record the video writer is halfway through.
                for (;;) {
                    std::vector<uint8_t> buf((size_t)drainChunk);
                    ssize_t n = ::recv(accepted, buf.data(), buf.size(), 0);
                    if (n <= 0) break;
                    {
                        std::lock_guard<std::mutex> lk(rxMutex);
                        rx.insert(rx.end(), buf.data(), buf.data() + n);
                    }
                    ++drainRounds;
                    if (!writersGo) {
                        // Release the second writer into the one instant when the socket can take
                        // bytes again — the round has just freed window, the kernel is about to
                        // report the space, and the metadata thread has been hot-spinning on this
                        // flag so its send() is already in flight when the video writer is woken.
                        auto tw = clock_::now();
                        while (!socketCanAccept(senderFd) && msSince(tw) < 200)
                            std::this_thread::yield();
                        writersGo = true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(drainEveryMs));
                }
            }
            finished = true;
        });
    }

    /// Close only the peer's READ half is not what this does: SHUT_WR closes the peer's SENDING
    /// half, so our side receives FIN (recv returns 0, receiverLoop sets connected_=false) while the
    /// peer keeps draining and the socket stays writable. That is the only way to clear the channel's
    /// alive flags without shutdown()/close() on the fd — and those two are what would otherwise
    /// break the blocked poll() for free and let the test pass with the alive check deleted.
    void halfClose() { if (accepted >= 0) ::shutdown(accepted, SHUT_WR); }

    void waitStarted() { while (!started && !finished) std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    size_t received() { std::lock_guard<std::mutex> lk(rxMutex); return rx.size(); }
    std::vector<uint8_t> take() { std::lock_guard<std::mutex> lk(rxMutex); return rx; }
    void stop() {
        stopNow = true;
        if (accepted >= 0) { shutdown(accepted, SHUT_RDWR); close(accepted); accepted = -1; }
        if (listenFd >= 0) { close(listenFd); listenFd = -1; }
        if (th.joinable()) th.join();
    }
};

int connectClient(unsigned short port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::connect(fd, (sockaddr*)&a, sizeof(a)) != 0) { ::perror("connect"); return -1; }
    return fd;
}

Channel* makeChannel(Peer& peer) {
    peer.start();
    int fd = connectClient(peer.port);
    peer.waitStarted();
    sockaddr_in me{};
    socklen_t ml = sizeof(me);
    getsockname(fd, (sockaddr*)&me, &ml);
    peer.senderFd = fd;      // GATED_DRAIN reads the window from this end
    return new Channel(fd, me, Constants::NETWORK_SEND_BUFFER, nullptr);
}

// ---------------------------------------------------------------------- what goes on the wire ---
const int kFrameBytes = 1536 * 1024;          // a real 1080p VMX keyframe is around this
const int kVideoFrames = 12;
const int kMetadataFrames = 12;
const int kXmlBytes = 40 * 1024;              // a chunky <Omtchannels> document

std::string hex(int v) {
    char b[8];
    snprintf(b, sizeof b, "%02x", v & 0xFF);
    return std::string(b);
}

uint8_t videoByte(int frame, size_t i) {
    return (uint8_t)(frame * 31 + i * 7 + (i >> 8));
}
char xmlChar(int frame, size_t i) {
    return (char)('a' + ((frame * 13 + i * 5 + (i >> 10)) % 26));
}

std::string xmlFor(int frame) {
    std::string s = "<Omtmetadata seq=\"" + std::to_string(frame) + "\">";
    s.reserve(kXmlBytes + 32);
    while ((int)s.size() < kXmlBytes) s.push_back(xmlChar(frame, s.size()));
    s += "</Omtmetadata>";
    return s;
}

struct SentVideo {
    FrameHeader hdr;
    VideoExtHeader ext;
    std::vector<uint8_t> payload;
};

SentVideo videoFor(int frame) {
    SentVideo v;
    v.payload.resize(kFrameBytes);
    for (size_t i = 0; i < v.payload.size(); ++i) v.payload[i] = videoByte(frame, i);
    v.ext = VideoExtHeader{};
    v.ext.codec = 0x31584D56;   // 'VMX1'
    v.ext.width = 1920; v.ext.height = 1080;
    v.ext.frameRateN = 30; v.ext.frameRateD = 1;
    v.ext.aspectRatio = 1.7778f; v.ext.colorSpace = 709;
    v.hdr = FrameHeader{};
    v.hdr.version = 1;
    v.hdr.frameType = (uint8_t)FrameType::Video;
    v.hdr.timestamp = 10000 * (frame + 1);
    v.hdr.dataLength = (int32_t)(VideoExtHeader::SIZE + v.payload.size());
    return v;
}

// ------------------------------------------------------------------ S1: one writer at a time ---
// The wire is a bare byte stream; the only thing that makes records out of it is that exactly one
// thread wrote it at a time. Parse it the way a subscriber does and check every record.
struct Wire {
    std::vector<uint8_t> b;
    size_t off = 0;
    int video = 0, metadata = 0;
    bool tornTail = false;
    std::string error;
    int firstMetadataAfterVideo = -1;
};

bool parseWire(Wire& w) {
    const uint8_t* p = w.b.data();
    const size_t n = w.b.size();
    while (w.off < n) {
        if (n - w.off < (size_t)FrameHeader::SIZE) { w.error = "short header at tail"; w.tornTail = true; return true; }
        FrameHeader h;
        memcpy(&h, p + w.off, sizeof(h));
        if (h.version != 1) { w.error = "record at offset " + std::to_string(w.off) +
                                        " claims version " + std::to_string(h.version); return false; }
        if (h.frameType != (uint8_t)FrameType::Video && h.frameType != (uint8_t)FrameType::Metadata) {
            w.error = "record at offset " + std::to_string(w.off) + " has unknown frameType " +
                      std::to_string(h.frameType) + " — the parse offset is wrong";
            return false;
        }
        const size_t total = (size_t)FrameHeader::SIZE + (size_t)h.dataLength;
        if (w.off + total > n) {           // a write cut short by the teardown: legal at the tail
            w.tornTail = true;
            return true;
        }
        const uint8_t* body = p + w.off + FrameHeader::SIZE;
        if (h.frameType == (uint8_t)FrameType::Video) {
            if ((size_t)h.dataLength < VideoExtHeader::SIZE) { w.error = "short video ext header"; return false; }
            VideoExtHeader e;
            memcpy(&e, body, sizeof(e));
            if (e.width != 1920 || e.height != 1080 || e.colorSpace != 709) {
                w.error = "video ext header at offset " + std::to_string(w.off) + " is not the one sent";
                return false;
            }
            const int frame = (int)(h.timestamp / 10000) - 1;
            const uint8_t* pay = body + VideoExtHeader::SIZE;
            const size_t payLen = (size_t)h.dataLength - VideoExtHeader::SIZE;
            for (size_t i = 0; i < payLen; ++i) {
                if (pay[i] != videoByte(frame, i)) {
                    w.error = "video record #" + std::to_string(frame) + " byte " + std::to_string(i) +
                              " is 0x" + hex(pay[i]) +
                              ", expected 0x" + hex(videoByte(frame, i)) +
                              " — foreign bytes were spliced into the payload";
                    return false;
                }
            }
            if (w.metadata > 0 && w.firstMetadataAfterVideo < 0) w.firstMetadataAfterVideo = (int)w.off;
            ++w.video;
        } else {
            const size_t payLen = (size_t)h.dataLength;
            const int frame = h.metadataLength ? 0 : -1; (void)frame;
            // Which document is it? Find the one whose seq it carries, then compare byte for byte.
            std::string got((const char*)body, payLen > 0 && body[payLen-1] == 0 ? payLen - 1 : payLen);
            size_t seqPos = got.find("seq=\"");
            if (seqPos == std::string::npos) { w.error = "metadata record at offset " + std::to_string(w.off) +
                                                         " has no seq — its bytes are not a whole document"; return false; }
            const int seq = atoi(got.c_str() + seqPos + 5);
            if (seq < 0 || seq >= kMetadataFrames) { w.error = "metadata seq out of range"; return false; }
            std::string want = xmlFor(seq);
            if (payLen != want.size() + 1 || memcmp(body, want.data(), want.size()) != 0 || body[want.size()] != 0) {
                size_t d = 0;
                while (d < want.size() && d < payLen && body[d] == (uint8_t)want[d]) ++d;
                w.error = "metadata record #" + std::to_string(seq) + " diverges at byte " + std::to_string(d) +
                          " of " + std::to_string(payLen) + " (0x" + hex(body[d]) +
                          " vs 0x" + hex((unsigned char)want[d]) +
                          ") — another writer cut in";
                return false;
            }
            ++w.metadata;
        }
        w.off += total;
    }
    return true;
}

// The bytes from the tear offset to the end of what the peer received must be exactly the first
// bytes of the record whose header is there: header, extension header, then the frame's own payload
// pattern. A second writer that got a turn after the tear shows up here as bytes that belong to a
// different record, which is the whole reason connected_ has to be cleared under the write lock.
bool tailIsOnlyTruncatedFrame(const Wire& w) {
    const size_t n = w.b.size();
    if (w.off + (size_t)FrameHeader::SIZE > n) return true;      // nothing but a partial header
    FrameHeader h;
    memcpy(&h, w.b.data() + w.off, sizeof(h));
    if (h.version != 1 || h.frameType != (uint8_t)FrameType::Video) return false;
    const uint8_t* body = w.b.data() + w.off + FrameHeader::SIZE;
    const size_t have = n - w.off - FrameHeader::SIZE;
    const size_t want = (size_t)h.dataLength;
    const size_t cmp = have < want ? have : want;
    if (have > want) return false;                              // MORE BYTES THAN THE RECORD CLAIMS
    VideoExtHeader e;
    if (cmp < VideoExtHeader::SIZE) return false;
    memcpy(&e, body, sizeof(e));
    if (e.width != 1920 || e.height != 1080) return false;
    const int frame = (int)(h.timestamp / 10000) - 1;
    const uint8_t* pay = body + VideoExtHeader::SIZE;
    const size_t payCmp = cmp - VideoExtHeader::SIZE;
    for (size_t i = 0; i < payCmp; ++i)
        if (pay[i] != videoByte(frame, i)) return false;
    return true;
}

void test_writers_are_serialised() {
    Peer peer;
    peer.mode = Peer::DRAIN_AFTER_STALL;
    peer.stallMs = 900;   // long enough that both writers are stuck mid-record when they unblock
    peer.rcvBuf = 0;      // ...and once it wakes, it must drain at full speed: a window-starved
                          // drain would make every 1.5 MB write outlast the metadata lock budget.
    Channel* ch = makeChannel(peer);

    std::atomic<int> accepted{0};
    auto videoThread = [&] {
        for (int i = 0; i < kVideoFrames; ++i) {
            SentVideo v = videoFor(i);
            if (ch->sendAsync(&v.hdr, sizeof(v.hdr), &v.ext, sizeof(v.ext),
                              v.payload.data(), v.payload.size()) > 0) ++accepted;
        }
    };
    auto metadataThread = [&] {
        for (int i = 0; i < kMetadataFrames; ++i) {
            std::string xml = xmlFor(i);
            ch->sendMetadataSync(xml.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    };

    std::thread t1(videoThread), t2(metadataThread);
    t1.join();
    // The metadata writer may still be blocked behind a stalled video write; that is the lock doing
    // its job, and it is bounded, so give it the budget plus the write deadline to finish.
    t2.join();

    // Let senderLoop finish what the pool is still holding: deleting the channel now would
    // interrupt an in-flight frame (which S3 tests on purpose) and leave the wire short.
    {
        // framesDropped is also charged by sendAsync() when it evicts a stale frame, so it cannot
        // be used to decide when the queue is empty. Watch the send counter settling instead.
        // senderLoop charges exactly one of {framesSent, tornSends} per buffer it dequeues, and
        // sendAsync charges staleEvicted for each frame it evicted before that. When those add up to
        // what the pool accepted, the queue is empty and the last byte is on the wire. (A "counter
        // stopped moving" heuristic cannot be used: during the peer's stall framesSent sits at zero
        // for a second, which is exactly when the write is most interesting.)
        auto tDrain = clock_::now();
        for (;;) {
            ChannelStats st = ch->getStats();
            int64_t done = st.framesSent + ch->tornSends();
            int64_t owed = accepted.load() - ch->staleEvicted();
            if (!ch->isConnected() || done >= owed || msSince(tDrain) > 15000) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::printf("      S1: pool handed %d frames to the sender; %lld sent, %lld dropped\n",
                    accepted.load(), (long long)ch->getStats().framesSent,
                    (long long)ch->getStats().framesDropped);
        std::printf("      S1: evicted=%lld torn=%lld connected=%d\n", (long long)ch->staleEvicted(),
                    (long long)ch->tornSends(), (int)ch->isConnected());
    }
    delete ch;
    peer.stop();

    Wire w;
    w.b = peer.take();
    bool parsed = parseWire(w);
    std::printf("      S1: %zu bytes on the wire, %d video + %d metadata records, %s\n",
                w.b.size(), w.video, w.metadata,
                parsed ? (w.tornTail ? "one torn record at the tail" : "clean") : w.error.c_str());
    ok(parsed, "S1: every record on the wire is whole — no writer spliced into another's frame");
    ok(w.video >= 2, "S1b: video frames really were in flight (the test is not vacuous)");
    ok(w.metadata >= 2, "S1c: metadata really was sent concurrently with them");
    ok(w.firstMetadataAfterVideo >= 0,
       "S1d: a metadata record follows a video record on the same stream — the two writers overlapped");
}

// ------------------------------------------------- S2: the whole write, not each poll, is bounded ---
// The peer takes 512 bytes every 250 ms. poll(POLLOUT) therefore succeeds forever, so a per-poll
// timeout never fires and the frame is in flight for minutes: the sender thread never returns to
// the queue and the pool slot it holds is never released. The bound has to be on the write.
const int kTrickleFrameBytes = 8 * 1024 * 1024;   // comfortably under Constants::VIDEO_MAX_SIZE

// The peer must keep poll(POLLOUT) succeeding — that is the whole point — while the transfer as a
// whole takes far longer than the deadline. 64 KB per read (at least one loopback MSS, so the
// receiver actually sends a window update rather than sitting on the space until a timer does it),
// every 400 ms, into a default-sized receive buffer: ~160 KB/s, and an 8 MB frame that the deadline
// has to cut off long before it would ever finish. A small SO_RCVBUF here was what made the first
// draft of this scenario pass against the pre-fix build: the window shut completely, no update came
// for over a second, and the per-poll timeout evicted the peer for the wrong reason.
void test_trickle_peer_is_evicted() {
    Peer peer;
    peer.mode = Peer::TRICKLE;
    peer.trickleBytes = 65536;
    peer.trickleEveryMs = 400;
    peer.rcvBuf = 0;
    Channel* ch = makeChannel(peer);

    SentVideo v = videoFor(0);
    v.payload.resize(kTrickleFrameBytes);
    for (size_t i = 0; i < v.payload.size(); ++i) v.payload[i] = videoByte(0, i);
    v.hdr.dataLength = (int32_t)(VideoExtHeader::SIZE + v.payload.size());
    ch->sendAsync(&v.hdr, sizeof(v.hdr), &v.ext, sizeof(v.ext), v.payload.data(), v.payload.size());

    auto t0 = clock_::now();
    while (ch->isConnected() && msSince(t0) < 8000)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool evicted = !ch->isConnected();
    std::printf("      S2: trickle peer (%d bytes / %d ms, frame %d MB) evicted %s after %ld ms "
                "(whole-write deadline is %d ms, per-poll slice is %d ms), %zu bytes arrived\n",
                peer.trickleBytes, peer.trickleEveryMs, kTrickleFrameBytes / (1024*1024),
                evicted ? "yes" : "NO — still in flight",
                msSince(t0), Constants::NETWORK_SEND_TIMEOUT_MS, Constants::NETWORK_SEND_TIMEOUT_MS,
                peer.received());
    ok(evicted, "S2: a peer that drains a trickle is evicted by the OVERALL write deadline");
    ok(ch->tornSends() >= 1, "S2b: the abandoned write was counted as a torn send, not silently dropped");

    auto t1 = clock_::now();
    delete ch;
    std::printf("      S2: ~Channel after the eviction took %ld ms\n", msSince(t1));
    ok(msSince(t1) < 3000, "S2c: destroying the evicted channel is quick");
    peer.stop();
}


// -------------------------------------------------- S3: teardown must be able to interrupt a write ---
// The property P0 #7 exists for: a write that is already in flight to a peer that is not going to
// take the rest of it must stop when the channel is stopped, not when the transfer finishes.
//
// Why this is staged the way it is. ~Channel() calls shutdown(SHUT_RDWR) before it joins the sender
// thread, and that alone breaks a blocked poll() — so "queue a frame, delete the channel, time the
// destructor" passes with the alive check deleted, and a scenario built that way is not evidence of
// anything (the first draft of this file had exactly that, and it stayed green against a build whose
// write loop never looked at running_ or connected_ at all). To observe the flags themselves the
// channel has to be marked dead by something that does NOT touch the descriptor: the peer's FIN,
// which receiverLoop turns into connected_=false while the write is still being served, 64 KB at a
// time, by a peer that is still draining.
//
// The second writer is in flight too, deliberately: it is what turns "the failure cleared connected_
// after it released the lock" into something observable on the wire.
void test_teardown_interrupts_an_in_flight_write() {
    Peer peer;
    peer.mode = Peer::TRICKLE;
    // Fast enough that the write is provably still in flight long before the 1 s write deadline
    // would tear it by itself (that is S2's job), slow enough that the 8 MB frame would take ~12 s
    // and the deleted-alive-check mutant is caught with it.
    peer.trickleBytes = 65536;
    peer.trickleEveryMs = 100;      // ~640 KB/s
    peer.rcvBuf = 0;
    Channel* ch = makeChannel(peer);

    SentVideo v = videoFor(3);
    v.payload.resize(kTrickleFrameBytes);
    for (size_t i = 0; i < v.payload.size(); ++i) v.payload[i] = videoByte(3, i);
    v.hdr.dataLength = (int32_t)(VideoExtHeader::SIZE + v.payload.size());
    ch->sendAsync(&v.hdr, sizeof(v.hdr), &v.ext, sizeof(v.ext), v.payload.data(), v.payload.size());

    // A metadata write in flight at the same time, on its own thread — the second writer the lock
    // exists to keep out of the way. It will be told to go once the video write is provably stuck.
    std::atomic<bool> metaDone{false};
    std::atomic<bool> metaResult{true};
    std::atomic<long> metaMs{-1};
    std::string xml = xmlFor(0);
    std::thread metaThread([&] {
        auto t = clock_::now();
        metaResult = ch->sendMetadataSync(xml.c_str());
        metaMs = msSince(t);
        metaDone = true;
    });

    // (0) prove the write is really in flight: bytes moving, channel still alive, frame unfinished.
    auto t0 = clock_::now();
    while (peer.received() < 100000 && ch->isConnected() && msSince(t0) < 10000)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const size_t receivedWhileAlive = peer.received();
    const bool aliveAtHalfClose = ch->isConnected();
    std::printf("      S3: %zu bytes of the %d MB frame accepted, channel alive=%d at the half-close\n",
                receivedWhileAlive, kTrickleFrameBytes / (1024 * 1024), (int)aliveAtHalfClose);
    ok(receivedWhileAlive >= 60000 && receivedWhileAlive < (size_t)kTrickleFrameBytes / 4,
       "S3a: the video write is in flight — bounded by the peer and nowhere near finished");
    // Separate assertion, because with no write serialisation the concurrent metadata call can
    // itself time out against the trickle peer and kill the channel before we ever get here. That is
    // a defect (it is what S1 is for), not a broken premise, and it must read as one and not the other.
    ok(aliveAtHalfClose, "S3a2: the channel was still connected when the peer sent its FIN");

    // (1) mark the channel dead without touching the fd.
    peer.halfClose();
    auto tDead = clock_::now();
    while (ch->isConnected() && msSince(tDead) < 4000)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const long sawDead = msSince(tDead);
    std::printf("      S3: connected_ cleared %ld ms after the peer sent FIN\n", sawDead);
    ok(!ch->isConnected(), "S3b: the peer's FIN marks the channel dead");

    // (2) THE GATE: after that, the write must stop. Fixed code notices the flag on its next loop
    // iteration — at most one poll slice (100 ms) plus one chunk in flight. With the alive check
    // removed it just keeps serving the frame: 160 KB/s for the next 45 seconds.
    const size_t atDeath = peer.received();
    auto tStop = clock_::now();
    while (ch->tornSends() < 1 && msSince(tStop) < 3000)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const long stopMs = msSince(tStop);
    const size_t afterDeath = peer.received();
    std::printf("      S3: the abandoned write was accounted for %ld ms after death; the peer took "
                "%zu more bytes in that time\n", stopMs, afterDeath - atDeath);
    ok(ch->tornSends() >= 1,
       "S3c: an in-flight write stops when the channel is marked dead (the alive flags are read)");
    // 3x the poll slice: the fixed code aborts on the next iteration of the write loop, the mutant
    // that never looks at the flags runs on to the 1 s write deadline instead — ~800 ms away here.
    ok(stopMs < 300,
       "S3d: it stops inside one poll slice, not when the peer finally finishes draining the frame");
    ok(afterDeath - atDeath <= (size_t)peer.trickleBytes * 2,
       "S3e: it stops writing — the peer takes at most a chunk or two after the channel is dead");

    // (3) the second writer must not get a turn in the gap. Whatever it managed to put on the wire
    // has to be a whole record that belonged there, never bytes trailing the torn frame.
    auto tMeta = clock_::now();
    while (!metaDone && msSince(tMeta) < Constants::NETWORK_METADATA_LOCK_BUDGET_MS + 2000)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::printf("      S3: the concurrent metadata write %s after %ld ms of its own (result=%d)\n",
                metaDone.load() ? "returned" : "NEVER RETURNED", metaMs.load(), (int)metaResult.load());
    ok(metaDone.load(), "S3f: the blocked second writer is released — it cannot be left holding the call");
    ok(!metaResult.load(), "S3f2: it reported failure — a dead channel must not claim it broadcast");
    if (metaDone.load()) metaThread.join(); else metaThread.detach();

    auto tKill = clock_::now();
    delete ch;
    std::printf("      S3: ~Channel took %ld ms with an 8 MB write in flight\n", msSince(tKill));
    ok(msSince(tKill) < 3000, "S3g: destroying the channel does not hang on the in-flight write");
    peer.stop();

    Wire w;
    w.b = peer.take();
    bool clean = parseWire(w);
    std::printf("      S3: %zu bytes, %d whole records, tear at offset %s\n", w.b.size(),
                w.video + w.metadata, std::to_string(w.off).c_str());
    ok(clean, "S3h: every record before the tear is whole — the second writer never cut into the frame");
    ok(w.tornTail && w.off + (size_t)FrameHeader::SIZE < w.b.size(),
       "S3i: the stream ends with the torn frame, exactly as an abandoned write must");
    // (b) from the brief: nothing at all may FOLLOW the partial record. Anything the second writer
    // appended after the tear is, by construction, past w.off + the truncated record's own bytes —
    // which the parser already walked as part of that record. Assert the tail is pure frame data.
    ok(tailIsOnlyTruncatedFrame(w),
       "S3j: no record of any kind follows the torn frame on the wire");
}

// ---------------------------------------- S4: the collision is forced, not waited for -----------
// S1 lets the two writers run at each other and hopes they interleave. When the kernel happened to
// let the video writer drain each record before the metadata writer got a slice of window, the
// unserialised mutant produced a clean wire and stayed green — a lock test whose verdict depends on
// thread timing is not evidence, and sw-mutant-video-unlocked was the proof of that here.
//
// This scenario does not hope. The peer reads NOTHING until the scenario has proved from outside —
// FIONREAD + TIOCOUTQ frozen, POLLOUT refused — that the video writer is parked mid-record with its
// send buffer full. The second writer is hot-spun and released into the rounds of window the peer
// then opens one recv() at a time, and the two compete for every round. With the lock the
// competition has exactly one entrant, so the wire can only ever hold whole records; without it the
// metadata writer takes a round and its bytes land inside the video record. The verdict comes from
// the byte-level parse and from the counters snapshotted at the instant the call returned, never
// from a wall-clock window: an 8 MB record at the peer's ~4 MB/s cannot finish inside the 1 s write
// bound, so the video writer is provably still mid-record when the metadata call is decided.
const int kS4MinRounds = 20;    // anti-vacuity: below this the rounds were not really opened

void test_blocked_video_write_excludes_the_metadata_writer() {
    Peer peer;
    peer.mode = Peer::GATED_DRAIN;
    peer.rcvBuf = 65536;          // small enough that the writer runs out of window mid-record
    peer.drainChunk = 32768;      // 4 MB/s: an 8 MB record cannot be served inside the 1 s bound,
    peer.drainEveryMs = 8;        // so the video writer is provably mid-record for the whole test
    Channel* ch = makeChannel(peer);

    SentVideo v = videoFor(7);
    v.payload.resize(kTrickleFrameBytes);
    for (size_t i = 0; i < v.payload.size(); ++i) v.payload[i] = videoByte(7, i);
    v.hdr.dataLength = (int32_t)(VideoExtHeader::SIZE + v.payload.size());
    ch->sendAsync(&v.hdr, sizeof(v.hdr), &v.ext, sizeof(v.ext), v.payload.data(), v.payload.size());

    // (1) wait until the video writer has put anything on the wire at all.
    auto t0 = clock_::now();
    while (ch->framesSent() == 0 && ch->tornSends() == 0 && queuedForReader(peer.accepted) <= 0 &&
           msSince(t0) < 5000)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

    // (2) PROVE the write is blocked rather than merely slow: the bytes on the wire do not move over
    // 50 ms, and the socket refuses POLLOUT. Everything after this is worthless unless it is true,
    // so it is asserted, not assumed — a peer whose window never shut would make the scenario
    // vacuously green, which is exactly the failure mode this file already had once.
    long pushed = -1;
    bool frozen = false;
    auto tTry = clock_::now();
    while (!frozen && msSince(tTry) < 4000) {
        const long a = queuedForReader(peer.accepted) + queuedForSender(peer.senderFd);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const long b = queuedForReader(peer.accepted) + queuedForSender(peer.senderFd);
        frozen = (a == b && a > 0);
        pushed = b;
    }
    const bool refused = !socketCanAccept(peer.senderFd);
    std::printf("      S4: %ld bytes of the %d MB record are on the wire, frozen=%d, POLLOUT refused=%d\n",
                pushed, kTrickleFrameBytes / (1024 * 1024), (int)frozen, (int)refused);
    ok(refused, "S4a: the socket refused POLLOUT — the video writer is parked in poll(), not draining");
    ok(frozen && pushed >= 4096 && pushed < (long)kTrickleFrameBytes / 4,
       "S4b: the video record is provably PART-written when the second writer is released "
       "(the collision is forced, not waited for)");

    // (3) The second writer, hot-spinning so its send() is already in flight when the peer's round
    // opens the window, and snapshotting the counters at the instant its call returns — reading
    // them after the join lets the video writer catch up and scores the broken build green.
    std::atomic<bool> metaDone{false};
    std::atomic<bool> metaSkipped{true};
    std::atomic<bool> metaResult{false};
    std::atomic<long> metaMs{-1};
    std::atomic<long> framesAtReturn{-1};
    std::atomic<long> tornAtReturn{-1};
    std::string xml = xmlFor(3);
    std::thread metaThread([&] {
        const auto tSpin = clock_::now();
        while (!peer.writersGo && msSince(tSpin) < 5000) std::this_thread::yield();
        if (!peer.writersGo) { metaDone = true; return; }   // S4e turns this into a red, not a skip
        metaSkipped = false;
        const auto t = clock_::now();
        const bool r = ch->sendMetadataSync(xml.c_str());
        framesAtReturn = ch->framesSent();
        tornAtReturn = ch->tornSends();
        metaResult = r;
        metaMs = msSince(t);
        metaDone = true;
    });

    peer.drainGo = true;
    auto tMeta = clock_::now();
    while (!metaDone && msSince(tMeta) <
           Constants::NETWORK_METADATA_LOCK_BUDGET_MS + Constants::NETWORK_SEND_TIMEOUT_MS + 3000)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::printf("      S4: %d window rounds opened; the metadata call %s after %ld ms "
                "(result=%d, framesSent=%ld torn=%ld at its return)\n",
                peer.drainRounds.load(),
                metaDone.load() ? (metaSkipped.load() ? "was SKIPPED" : "returned") : "NEVER RETURNED",
                metaMs.load(), (int)metaResult.load(), framesAtReturn.load(), tornAtReturn.load());
    ok(!metaSkipped.load() && metaDone.load(),
       "S4d: the second writer was released into the open window and its call returned — it was "
       "neither skipped nor left holding the caller");

    // (4) let the peer take everything the sender managed to write before the channel died, so the
    // parse sees the whole stream, then tear down.
    auto tDrain = clock_::now();
    while ((ch->isConnected() || queuedForReader(peer.accepted) > 0) && msSince(tDrain) < 5000)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (metaThread.joinable()) metaThread.join();
    // Counted here, not at the metadata call's return: the mutant's writer can finish in two rounds
    // and still have the peer open a hundred more afterwards, and it is the total number of rounds
    // the two writers actually competed for that makes this a forced collision.
    std::printf("      S4: %d window rounds opened in total\n", peer.drainRounds.load());
    ok(peer.writersGo.load() && peer.drainRounds.load() >= kS4MinRounds,
       "S4e: the peer really opened the window kS4MinRounds times — the two writers were made to "
       "compete for it, this is not a vacuous pass");
    delete ch;
    peer.stop();

    Wire w;
    w.b = peer.take();
    bool parsed = parseWire(w);
    std::printf("      S4: %zu bytes drained, %d whole video + %d whole metadata records, %s\n",
                w.b.size(), w.video, w.metadata,
                parsed ? (w.tornTail ? "one torn record at the tail" : "clean") : w.error.c_str());
    ok(parsed, "S4f: every record on the wire is whole — the metadata writer never cut into the "
               "video record the video writer was blocked inside");
    ok(!(metaResult.load() && framesAtReturn.load() == 0),
       "S4g: the metadata call never COMPLETED while a video record was still in flight — it either "
       "waited for the write lock or reported failure");
    ok(!metaResult.load(),
       "S4h: with the video writer owning the socket, the blocked broadcast reported failure "
       "instead of claiming it went out");
    ok(!parsed || tailIsOnlyTruncatedFrame(w),
       "S4i: nothing follows the torn video record — the failure cleared connected_ under the lock");
}

} // namespace

int main() {
    // Line-buffer the log: two of these three scenarios fail by hanging, and a killed process never
    // flushes a block buffer — the run log comes back empty and the hang is undiagnosable.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::printf("socket-write-order: one writer per channel, one deadline per write\n");

    test_writers_are_serialised();
    test_trickle_peer_is_evicted();
    test_teardown_interrupts_an_in_flight_write();
    test_blocked_video_write_excludes_the_metadata_writer();

    std::printf("\nsocket-write-order: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
