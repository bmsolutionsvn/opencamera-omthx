/*
 * receiver-peer-lifetime.cpp — what the receiver thread is allowed to conclude from a read that
 * returned nothing.
 *
 * The defect (qa-ompin/FOREIGN-IN-DIAGNOSTICS-20261015.md, scenarios D1 and D4, measured against
 * this file at 3d64d14a): receiverLoop()'s read result was handled as
 *
 *     ssize_t received = recv(socketFd_, ..., 0);
 *     if (received <= 0) {
 *         if (received == 0) LOGI("connection closed by peer");
 *         else if (errno != EAGAIN && errno != EWOULDBLOCK) LOGE("recv error %d", errno);
 *         connected_ = false;      // <- the errno test only picked a log line, never an outcome
 *         break;
 *     }
 *
 * Two perfectly healthy peers died there, both measured, both reproduced below:
 *
 *   D1  a subscriber that shutdown(SHUT_WR)s after subscribing — the pure-receiver move that OBS,
 *       libomtnet and our own OmpIn all make — got the 86-byte greeting and NOTHING else: 86
 *       bytes / 0 frames against 3.9 MB / 179 frames for the byte-identical peer with no
 *       half-close. recv() answers 0 forever once the peer's write half is gone, and 0 was read as
 *       "the peer is gone". The two halves of a TCP connection are independent; this file exists
 *       to push video at a peer that has nothing left to say.
 *   D4  one spurious readability (select() said readable, the bytes were already taken) makes
 *       recv() answer EAGAIN, and the channel died before the SUBSCRIBE itself registered — so the
 *       sender never fans a single frame out to a viewer it still lists as connected.
 *
 * It is also silent, which is how it survived an audit cycle: only ~Channel() closes the
 * descriptor, and that runs from cleanupDisconnected(), which is only reached from
 * Sender::send()/getStats(). A viewer that is getting nothing has no way to say so.
 *
 * HOW THE EAGAIN IS FORCED (not raced for): recv() is interposed below. The harness declares "the
 * next N reads find nothing" and the receiver thread then genuinely sees recv()==-1/EAGAIN on a
 * socket select() called readable, N times, with the bytes still queued behind it. A competing
 * reader would have to win a microsecond window to produce one EAGAIN and would tear the frame
 * apart when it lost; the seam produces the exact kernel answer, every run, and counts it. (Same
 * link-time-seam precedent as qa/stub_vmx.cpp.)
 *
 * Every case runs in its own forked child under a wall-clock deadline: the failure mode of a
 * channel that never dies is a spin, and a child that has not exited inside the deadline is killed
 * and scored RED rather than looking like a run that is still going.
 *
 * What is NOT changed here, and is asserted against: the slow-subscriber eviction (qa/
 * slow-subscriber.cpp pins it) and the write-side deadline (qa/socket-write-order.cpp pins it).
 * "Keep the channel alive on a benign read" must not become "never reap a dead one", which is why
 * the RST case and the full-close case are in the same file as the two keep-alive cases.
 *
 * Build + run on the host (no device, no JVM):  bash qa/build-host.sh
 * Expected: ALL PASS on the fixed tree; against the pre-fix OMTChannel.cpp the two keep-alive cases
 * are red — that A/B is what proves the gate has teeth.
 */
#include "OMTChannel.h"
#include "OMTConstants.h"
#include "OMTProtocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace omt;
using clock_ = std::chrono::steady_clock;

// ---------------------------------------------------------------------------------------------
// The recv() seam. OMTChannel.cpp is a separate TU, so its call to recv() binds to this
// definition; everything the harness itself reads goes through rawRecv() below and is untouched.
// ---------------------------------------------------------------------------------------------
static std::atomic<int> g_inject_eagain{0};    // reads that must answer -1/EAGAIN
static std::atomic<int> g_inject_eof{0};       // reads that must answer 0 (read-half EOF)
static std::atomic<int> g_injected_seen{0};    // how many the receiver actually took
static std::atomic<int> g_inject_eof_seen{0};

extern "C" ssize_t recv(int fd, void* buf, size_t len, int flags) {
    int n = g_inject_eagain.load(std::memory_order_relaxed);
    if (n > 0) {
        g_inject_eagain.fetch_sub(1, std::memory_order_relaxed);
        g_injected_seen.fetch_add(1, std::memory_order_relaxed);
        errno = EAGAIN;
        return -1;
    }
    n = g_inject_eof.load(std::memory_order_relaxed);
    if (n > 0) {
        g_inject_eof.fetch_sub(1, std::memory_order_relaxed);
        g_inject_eof_seen.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    return syscall(SYS_recvfrom, fd, buf, len, flags, nullptr, nullptr);
}

static ssize_t rawRecv(int fd, void* buf, size_t len, int flags) {
    return syscall(SYS_recvfrom, fd, buf, len, flags, nullptr, nullptr);
}

// ---------------------------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------------------------
static long msSince(clock_::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_::now() - t0).count();
}

static int g_fail = 0;
static void check(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fail;
}
static void checkf(bool cond, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void checkf(bool cond, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[512]; vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    check(cond, buf);
}

/// The subscriber side: a real TCP peer that connects, sends control frames, reads everything the
/// sender writes, and counts complete records.
struct Peer {
    int client = -1;
    std::atomic<bool> stop{false};
    std::atomic<int> videoFrames{0};
    std::atomic<int> metaFrames{0};
    std::atomic<long long> bytes{0};
    std::thread reader;

    static int connectTo(unsigned short port) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        if (::connect(fd, (sockaddr*)&a, sizeof(a)) != 0) { ::perror("connect"); ::close(fd); return -1; }
        return fd;
    }

    void startReading() {
        reader = std::thread([this] {
            std::vector<uint8_t> buf;
            buf.reserve(1 << 20);
            uint8_t chunk[64 * 1024];
            while (!stop.load()) {
                struct pollfd p{client, POLLIN, 0};
                int pr = ::poll(&p, 1, 20);
                if (pr < 0) { if (errno == EINTR) continue; break; }
                if (pr == 0) continue;
                ssize_t n = rawRecv(client, chunk, sizeof chunk, 0);
                if (n == 0) break;                       // sender closed its half
                if (n < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) continue; break; }
                bytes += (long long)n;
                buf.insert(buf.end(), chunk, chunk + n);
                // Framer over the OMT wire: 16-byte header, dataLength bytes behind it.
                size_t pos = 0;
                while (buf.size() - pos >= FrameHeader::SIZE) {
                    FrameHeader h;
                    memcpy(&h, buf.data() + pos, FrameHeader::SIZE);
                    if (h.dataLength < 0) return;         // hostile: stop counting, the case asserts
                    const size_t total = FrameHeader::SIZE + (size_t)h.dataLength;
                    if (buf.size() - pos < total) break;  // need more
                    if (h.version != 1) return;
                    if (h.frameType == 2) ++videoFrames; else if (h.frameType == 1) ++metaFrames;
                    pos += total;
                }
                if (pos) buf.erase(buf.begin(), buf.begin() + pos);
            }
        });
    }

    /// One control (Metadata) frame, in the sender's own dialect: NUL-terminated, metadataLength
    /// == dataLength == strlen+1 — the same bytes Sender::sendMetadataSync puts on the wire.
    bool sendMeta(const char* xml) {
        const size_t len = strlen(xml);
        FrameHeader h{};
        h.version = 1;
        h.frameType = 1;  // Metadata
        h.timestamp = 0;
        h.metadataLength = (uint16_t)(len + 1);
        h.dataLength = (int32_t)(len + 1);
        std::vector<uint8_t> out(sizeof(FrameHeader) + len + 1);
        memcpy(out.data(), &h, sizeof(FrameHeader));
        memcpy(out.data() + sizeof(FrameHeader), xml, len);
        out[sizeof(FrameHeader) + len] = 0;
        size_t off = 0;
        while (off < out.size()) {
            ssize_t w = send(client, out.data() + off, out.size() - off, MSG_NOSIGNAL);
            if (w <= 0) return false;
            off += (size_t)w;
        }
        return true;
    }

    /// The pure-receiver move: hang up the half we never intended to write from, keep reading.
    void halfClose() { ::shutdown(client, SHUT_WR); }

    /// A peer that is simply gone: RST, not FIN.
    void reset() {
        struct linger l{1, 0};
        setsockopt(client, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
        ::close(client); client = -1;
    }
    void closeGracefully() { ::close(client); client = -1; }

    void finish() {
        stop = true;
        if (reader.joinable()) reader.join();
    }
};

/// Records every listener callback, so "the channel was declared dead" is observable from outside
/// the class, not just through isConnected().
struct CountingListener : public IChannelListener {
    std::atomic<int> disconnected{0};
    std::atomic<int> tally{0};
    std::atomic<int> quality{0};
    void onDisconnected(Channel*) override { ++disconnected; }
    void onTallyChanged(Channel*) override { ++tally; }
    void onQualityChanged(Channel*) override { ++quality; }
};

/// The listener's onDisconnected is called from the receiver thread; give it a moment to land so a
/// PASS is not just a thread that has not run yet.
static void settle(int ms) {
    const auto t0 = clock_::now();
    while (msSince(t0) < ms) std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

/// Frame payload that cannot be absorbed by a kernel buffer silently: pseudo-random, so a
/// truncated record is a truncated record and not an unlucky run of zeros.
static std::vector<uint8_t> noisyPayload(size_t n, unsigned seed) {
    std::vector<uint8_t> v(n);
    uint32_t x = seed * 2654435761u + 1u;
    for (size_t i = 0; i < n; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; v[i] = (uint8_t)(x >> 24); }
    return v;
}

/// Push exactly `want` video frames through the production path and wait for each to leave the
/// channel. sendAsync() is a queue hand-off with a 4-slot ring and a drop-oldest policy, so
/// bursting 20 of them would evict 16 from the queue and prove nothing about the wire; pacing on
/// framesSent_ is what makes "the subscriber received N frames" the same statement as
/// "the sender wrote N frames".
static bool pumpFrames(Channel* ch, size_t payloadSize, int want) {
    FrameHeader hdr{};
    hdr.version = 1;
    hdr.frameType = 2;  // Video
    hdr.timestamp = 0;
    hdr.metadataLength = 0;
    hdr.dataLength = (int32_t)(sizeof(VideoExtHeader) + payloadSize);
    VideoExtHeader ext{};
    ext.codec = Codec::VMX1;
    ext.width = 512; ext.height = 512; ext.frameRateN = 30; ext.frameRateD = 1;
    ext.aspectRatio = 1.0f; ext.flags = 0; ext.colorSpace = 709;
    for (int i = 0; i < want; ++i) {
        std::vector<uint8_t> payload = noisyPayload(payloadSize, (unsigned)i + 7u);
        const auto t0 = clock_::now();
        ch->sendAsync(&hdr, sizeof(hdr), &ext, sizeof(ext), payload.data(), payload.size());
        while (ch->framesSent() < i + 1 && ch->isConnected() && msSince(t0) < 2000)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (ch->framesSent() < i + 1) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// the cases
// ---------------------------------------------------------------------------------------------

/// D1 — the half-closing subscriber is a pure receiver, not a dead channel.
static int case_half_close_keeps_streaming() {
    std::printf("\ncase half-close-keeps-streaming (D1: shutdown(SHUT_WR) after subscribing)\n");
    int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    bind(listenFd, (sockaddr*)&a, sizeof(a));
    socklen_t sl = sizeof(a); getsockname(listenFd, (sockaddr*)&a, &sl);
    listen(listenFd, 4);

    Peer peer;
    peer.client = Peer::connectTo(ntohs(a.sin_port));
    if (peer.client < 0) return 1;
    int accepted = ::accept(listenFd, nullptr, nullptr);
    if (accepted < 0) return 1;
    sockaddr_in peerAddr{}; socklen_t pl = sizeof(peerAddr);
    getpeername(accepted, (sockaddr*)&peerAddr, &pl);
    peer.startReading();

    CountingListener listener;
    Channel* ch = new Channel(accepted, peerAddr, Constants::NETWORK_SEND_BUFFER, &listener);

    // The accept loop's greeting, exactly as Sender::acceptLoop() writes it.
    ch->sendMetadataSync("<OMTInfo ProductName=\"QA\" Manufacturer=\"Hermes\" Version=\"1.0\" />");

    check(peer.sendMeta(MetadataConstants::CHANNEL_SUBSCRIBE_VIDEO), "the subscribe frame reached the sender");
    const auto t0 = clock_::now();
    while (!ch->isVideoSubscribed() && msSince(t0) < 1000)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(ch->isVideoSubscribed(), "precondition: the subscription registered before the half-close");

    // ---- the defect: hang up the write half, keep the read half (what OBS/libomtnet/OmpIn do) --
    peer.halfClose();
    // Several 100 ms select ticks: the old code died on the first recv() that answered 0.
    settle(400);

    check(ch->isConnected(),
          "the channel SURVIVES the peer closing only its write half (pre-fix: recv()==0 was death)");
    check(listener.disconnected.load() == 0,
          "no onDisconnected for a half-close — the listener is told about deaths, not about states");

    // ---- and it must still STREAM to that peer, which is the entire point of the channel -------
    const int want = 20;
    const bool pumped = pumpFrames(ch, 256 * 1024, want);
    const auto t1 = clock_::now();
    while (peer.videoFrames.load() < want && msSince(t1) < 3000)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::printf("      sender wrote %lld frames / %lld bytes; peer read %d video records / %lld bytes\n",
                (long long)ch->framesSent(), (long long)ch->getStats().bytesSent,
                peer.videoFrames.load(), (long long)peer.bytes.load());
    check(pumped, "every frame left the channel (pre-fix: sendAsync() returns 0 on a dead channel)");
    check(peer.videoFrames.load() == want,
          "the half-closed subscriber RECEIVES the whole run (pre-fix: 86-byte greeting, 0 frames)");
    check(ch->isConnected(), "still connected after streaming — the eviction is not where this ends");

    ch->disconnect();
    delete ch;
    peer.finish();
    if (peer.client >= 0) { ::shutdown(peer.client, SHUT_RDWR); ::close(peer.client); peer.client = -1; }
    ::close(listenFd);
    return g_fail ? 1 : 0;
}

/// D1b — the same peer with no half-close: the control that keeps the keep-alive fix honest. If
/// this one goes red, the fix broke the ordinary path rather than fixing the unusual one.
static int case_no_half_close_control() {
    std::printf("\ncase no-half-close-control (D1b: identical peer, no shutdown)\n");
    int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    bind(listenFd, (sockaddr*)&a, sizeof(a));
    socklen_t sl = sizeof(a); getsockname(listenFd, (sockaddr*)&a, &sl);
    listen(listenFd, 4);

    Peer peer;
    peer.client = Peer::connectTo(ntohs(a.sin_port));
    if (peer.client < 0) return 1;
    int accepted = ::accept(listenFd, nullptr, nullptr);
    if (accepted < 0) return 1;
    sockaddr_in peerAddr{}; socklen_t pl = sizeof(peerAddr);
    getpeername(accepted, (sockaddr*)&peerAddr, &pl);
    peer.startReading();

    CountingListener listener;
    Channel* ch = new Channel(accepted, peerAddr, Constants::NETWORK_SEND_BUFFER, &listener);
    check(peer.sendMeta(MetadataConstants::CHANNEL_SUBSCRIBE_VIDEO), "subscribe sent");
    const auto t0 = clock_::now();
    while (!ch->isVideoSubscribed() && msSince(t0) < 1000)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(ch->isVideoSubscribed(), "subscription registered");
    check(pumpFrames(ch, 256 * 1024, 10), "10 frames pumped");
    const auto t1 = clock_::now();
    while (peer.videoFrames.load() < 10 && msSince(t1) < 3000)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(peer.videoFrames.load() == 10, "the ordinary subscriber still gets all 10 frames");
    check(ch->isConnected(), "and stays connected");
    (void)listener;

    ch->disconnect();
    delete ch;
    peer.finish();
    if (peer.client >= 0) { ::shutdown(peer.client, SHUT_RDWR); ::close(peer.client); peer.client = -1; }
    ::close(listenFd);
    return g_fail ? 1 : 0;
}

/// D4 — a spurious readability: select() says readable, recv() finds nothing. Forced through the
/// seam, so it happens every run rather than when the scheduler feels like it.
static int case_spurious_eagain_keeps_channel() {
    std::printf("\ncase spurious-eagain-keeps-channel (D4: recv()==-1/EAGAIN after a readable select)\n");
    int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    bind(listenFd, (sockaddr*)&a, sizeof(a));
    socklen_t sl = sizeof(a); getsockname(listenFd, (sockaddr*)&a, &sl);
    listen(listenFd, 4);

    Peer peer;
    peer.client = Peer::connectTo(ntohs(a.sin_port));
    if (peer.client < 0) return 1;
    int accepted = ::accept(listenFd, nullptr, nullptr);
    if (accepted < 0) return 1;
    sockaddr_in peerAddr{}; socklen_t pl = sizeof(peerAddr);
    getpeername(accepted, (sockaddr*)&peerAddr, &pl);
    peer.startReading();

    CountingListener listener;
    Channel* ch = new Channel(accepted, peerAddr, Constants::NETWORK_SEND_BUFFER, &listener);

    // Arm the seam BEFORE the bytes land: the receiver's first reads on this socket must all answer
    // EAGAIN, with the subscribe frame still queued behind them. Four, so the case is not about one
    // lucky tick; the guard that reaps a socket which never delivers is 100, so four must be free.
    g_injected_seen = 0;
    g_inject_eagain = 4;
    check(peer.sendMeta(MetadataConstants::CHANNEL_SUBSCRIBE_VIDEO), "subscribe sent while the seam was armed");

    const auto t0 = clock_::now();
    while ((g_injected_seen.load() < 4 || !ch->isVideoSubscribed()) && msSince(t0) < 3000)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const int seen = g_injected_seen.load();
    std::printf("      seam answered EAGAIN %d time(s); subscribed=%s; connected=%s\n",
                seen, ch->isVideoSubscribed() ? "yes" : "NO", ch->isConnected() ? "yes" : "NO");
    check(seen >= 4, "precondition: the receiver really was handed EAGAIN on a readable socket "
                    "(a case that never injected is not evidence of anything)");
    check(ch->isConnected(), "the channel SURVIVES the transient read (pre-fix: the subscribe never "
                            "registered and the viewer got nothing for the rest of the connection)");
    check(ch->isVideoSubscribed(), "and the subscription it was waiting for still registers");
    check(listener.disconnected.load() == 0, "no onDisconnected for a transient read");

    // The proof that matters to a viewer: the channel is not merely alive, it streams.
    check(pumpFrames(ch, 256 * 1024, 5), "5 frames pumped after the storm of transient reads");
    const auto t1 = clock_::now();
    while (peer.videoFrames.load() < 5 && msSince(t1) < 3000)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(peer.videoFrames.load() == 5, "the peer receives them all");

    ch->disconnect();
    delete ch;
    peer.finish();
    if (peer.client >= 0) { ::shutdown(peer.client, SHUT_RDWR); ::close(peer.client); peer.client = -1; }
    ::close(listenFd);
    return g_fail ? 1 : 0;
}

/// The other side of "keep the channel alive": a socket that answers EAGAIN forever is not a slow
/// viewer, it is a dead one, and looping on it pins a core for the life of the app. Same seam,
/// past the streak limit.
static int case_transient_forever_is_reaped() {
    std::printf("\ncase transient-forever-is-reaped (the keep-alive must not become a leak)\n");
    int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    bind(listenFd, (sockaddr*)&a, sizeof(a));
    socklen_t sl = sizeof(a); getsockname(listenFd, (sockaddr*)&a, &sl);
    listen(listenFd, 4);

    Peer peer;
    peer.client = Peer::connectTo(ntohs(a.sin_port));
    if (peer.client < 0) return 1;
    int accepted = ::accept(listenFd, nullptr, nullptr);
    if (accepted < 0) return 1;
    sockaddr_in peerAddr{}; socklen_t pl = sizeof(peerAddr);
    getpeername(accepted, (sockaddr*)&peerAddr, &pl);
    peer.startReading();

    CountingListener listener;
    Channel* ch = new Channel(accepted, peerAddr, Constants::NETWORK_SEND_BUFFER, &listener);
    g_injected_seen = 0;
    g_inject_eagain = 100000;              // every read the receiver can make, forever
    check(peer.sendMeta(MetadataConstants::CHANNEL_SUBSCRIBE_VIDEO), "subscribe sent");

    const auto t0 = clock_::now();
    while (ch->isConnected() && msSince(t0) < 4000)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const bool reaped = !ch->isConnected();
    std::printf("      %d transient reads injected; channel %s after %ld ms\n",
                g_injected_seen.load(), reaped ? "reaped" : "STILL ALIVE", msSince(t0));
    check(g_injected_seen.load() >= 100, "precondition: the seam really starved the receiver");
    check(reaped, "a socket that never delivers is eventually reaped — the transient path is "
                  "bounded, so 'keep the peer' cannot become 'spin a core forever'");
    (void)listener;

    ch->disconnect();
    delete ch;
    peer.finish();
    g_inject_eagain = 0;
    if (peer.client >= 0) { ::shutdown(peer.client, SHUT_RDWR); ::close(peer.client); peer.client = -1; }
    ::close(listenFd);
    return g_fail ? 1 : 0;
}

/// A peer that really is gone must still die inside a second. ECONNRESET is on the peer-gone list;
/// this is the case that stops the fix from being "never disconnect anything".
static int case_rst_still_dies() {
    std::printf("\ncase rst-still-dies (hard socket error is still death)\n");
    int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    bind(listenFd, (sockaddr*)&a, sizeof(a));
    socklen_t sl = sizeof(a); getsockname(listenFd, (sockaddr*)&a, &sl);
    listen(listenFd, 4);

    Peer peer;
    peer.client = Peer::connectTo(ntohs(a.sin_port));
    if (peer.client < 0) return 1;
    int accepted = ::accept(listenFd, nullptr, nullptr);
    if (accepted < 0) return 1;
    sockaddr_in peerAddr{}; socklen_t pl = sizeof(peerAddr);
    getpeername(accepted, (sockaddr*)&peerAddr, &pl);
    // deliberately NOT reading: the RST must arrive on the receive path, not be consumed away

    CountingListener listener;
    Channel* ch = new Channel(accepted, peerAddr, Constants::NETWORK_SEND_BUFFER, &listener);
    check(ch->isConnected(), "precondition: the channel starts connected");
    peer.reset();   // SO_LINGER{1,0} + close -> RST

    const auto t0 = clock_::now();
    while (ch->isConnected() && msSince(t0) < 2000)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::printf("      channel disconnected %s after %ld ms\n",
                ch->isConnected() ? "NEVER" : "yes", msSince(t0));
    check(!ch->isConnected(), "ECONNRESET on the read path still tears the channel down within 2 s");
    const auto t1 = clock_::now();
    while (listener.disconnected.load() == 0 && msSince(t1) < 1000)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(listener.disconnected.load() >= 1, "and the listener is told, so the channel gets reaped");

    ch->disconnect();
    delete ch;
    ::close(listenFd);
    return g_fail ? 1 : 0;
}

/// The half-close must not turn the channel into a zombie that outlives the peer. Once the peer is
/// fully gone, the first write that the kernel refuses has to end the channel — that is what makes
/// "do not die on EOF" safe rather than a leak.
static int case_full_close_is_still_reaped() {
    std::printf("\ncase full-close-is-still-reaped (EOF keeps the channel; the send error ends it)\n");
    int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    bind(listenFd, (sockaddr*)&a, sizeof(a));
    socklen_t sl = sizeof(a); getsockname(listenFd, (sockaddr*)&a, &sl);
    listen(listenFd, 4);

    Peer peer;
    peer.client = Peer::connectTo(ntohs(a.sin_port));
    if (peer.client < 0) return 1;
    int accepted = ::accept(listenFd, nullptr, nullptr);
    if (accepted < 0) return 1;
    sockaddr_in peerAddr{}; socklen_t pl = sizeof(peerAddr);
    getpeername(accepted, (sockaddr*)&peerAddr, &pl);
    peer.startReading();

    CountingListener listener;
    Channel* ch = new Channel(accepted, peerAddr, Constants::NETWORK_SEND_BUFFER, &listener);
    check(peer.sendMeta(MetadataConstants::CHANNEL_SUBSCRIBE_VIDEO), "subscribe sent");
    const auto t0 = clock_::now();
    while (!ch->isVideoSubscribed() && msSince(t0) < 1000)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

    peer.finish();                              // stop reading
    peer.closeGracefully();                     // FIN — the receiver will see recv() == 0
    settle(300);
    check(ch->isConnected(), "the EOF itself does not kill it (that is the fix)");

    // ...so the only thing that can reclaim it is the send side, and it must.
    const auto t1 = clock_::now();
    int pushed = 0;
    while (ch->isConnected() && msSince(t1) < 5000 && pushed < 40) {
        pumpFrames(ch, 256 * 1024, 1);
        ++pushed;
        settle(50);
    }
    std::printf("      after %d pushes the channel is %s (%ld ms)\n", pushed,
                ch->isConnected() ? "STILL CONNECTED" : "gone", msSince(t1));
    check(!ch->isConnected(), "a fully-closed peer is still reclaimed once frames are pushed at it "
                              "(EOF keeps the channel; the failing write ends it)");

    ch->disconnect();
    delete ch;
    ::close(listenFd);
    return g_fail ? 1 : 0;
}

/// The once-per-episode rule, counted rather than sampled: N transient reads with no byte in
/// between must produce ONE warning line, and after a read that really delivers, the next stall must
/// be able to speak up again. The latch itself cannot be sampled from a test — the successful read
/// that re-arms it lands microseconds later — so the counter is the observable.
static int case_warn_counts_per_episode() {
    std::printf("\ncase warn-counts-per-episode (once per stall episode, not per attempt)\n");
    int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    bind(listenFd, (sockaddr*)&a, sizeof(a));
    socklen_t sl = sizeof(a); getsockname(listenFd, (sockaddr*)&a, &sl);
    listen(listenFd, 4);

    Peer peer;
    peer.client = Peer::connectTo(ntohs(a.sin_port));
    if (peer.client < 0) return 1;
    int accepted = ::accept(listenFd, nullptr, nullptr);
    if (accepted < 0) return 1;
    sockaddr_in peerAddr{}; socklen_t pl = sizeof(peerAddr);
    getpeername(accepted, (sockaddr*)&peerAddr, &pl);
    peer.startReading();

    CountingListener listener;
    Channel* ch = new Channel(accepted, peerAddr, Constants::NETWORK_SEND_BUFFER, &listener);
    check(ch->readTransientWarningsForTest() == 0, "precondition: nothing warned about yet");

    // ---- episode 1: 50 transient reads, no byte in between -> exactly ONE line -----------------
    // 50 is deliberately below the 100-streak reaper, so the case measures the logging rule and not
    // the spin guard. The per-attempt form would print 50 lines here; the desktop's printed 802 in
    // three minutes against one slow OBS.
    constexpr int kAttempts = 50;
    g_injected_seen = 0;
    g_inject_eagain = kAttempts;
    peer.sendMeta(MetadataConstants::CHANNEL_SUBSCRIBE_VIDEO);
    const auto t0 = clock_::now();
    while ((!ch->isVideoSubscribed() || g_injected_seen.load() < kAttempts) && msSince(t0) < 3000)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const int64_t afterFirst = ch->readTransientWarningsForTest();
    std::printf("      %d transient reads in one episode -> %lld warning line(s); subscribed=%s\n",
                g_injected_seen.load(), (long long)afterFirst, ch->isVideoSubscribed() ? "yes" : "NO");
    check(g_injected_seen.load() >= kAttempts, "precondition: the receiver really was handed EAGAIN "
                                               "that many times (a case that never injected proves nothing)");
    check(ch->isVideoSubscribed(), "the subscribe behind the episode still registers");
    check(afterFirst == 1, "ONE line for the whole episode, not one per attempt");
    check(ch->isConnected(), "and the channel is alive");

    // ---- episode 2: a read that delivers closes the episode, so the next one must be printed ----
    g_injected_seen = 0;
    g_inject_eagain = kAttempts;
    peer.sendMeta(MetadataConstants::TALLY_PROGRAM);
    const auto t1 = clock_::now();
    while ((!ch->isTallyProgram() || g_injected_seen.load() < kAttempts) && msSince(t1) < 3000)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const int64_t afterSecond = ch->readTransientWarningsForTest();
    std::printf("      second episode -> %lld warning line(s) total\n", (long long)afterSecond);
    check(ch->isTallyProgram(), "precondition: a read between the episodes really delivered bytes "
                                "(that is what re-arms the warning)");
    check(afterSecond == 2, "the new stall is reported — a warning that latches forever is just a "
                            "commented-out log line");
    (void)listener;

    ch->disconnect();
    delete ch;
    peer.finish();
    if (peer.client >= 0) { ::shutdown(peer.client, SHUT_RDWR); ::close(peer.client); peer.client = -1; }
    ::close(listenFd);
    return g_fail ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// runner: one forked child per case, scored RED if it does not exit inside the deadline
// ---------------------------------------------------------------------------------------------
struct Case { const char* name; int (*fn)(); int budgetMs; };
static const Case kCases[] = {
    {"half-close-keeps-streaming",     case_half_close_keeps_streaming,     15000},
    {"no-half-close-control",          case_no_half_close_control,          15000},
    {"spurious-eagain-keeps-channel",  case_spurious_eagain_keeps_channel,  15000},
    {"transient-forever-is-reaped",    case_transient_forever_is_reaped,    15000},
    {"rst-still-dies",                 case_rst_still_dies,                 15000},
    {"full-close-is-still-reaped",     case_full_close_is_still_reaped,     20000},
    {"warn-counts-per-episode",         case_warn_counts_per_episode,    15000},
};

static int run_case(const Case& c) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) { std::perror("fork"); return 1; }
    if (pid == 0) {
        g_fail = 0;
        int rc = 1;
        try { rc = c.fn(); }
        catch (const std::exception& e) { std::printf("  [FAIL] case threw: %s\n", e.what()); rc = 1; }
        catch (...) { std::printf("  [FAIL] case threw a non-standard exception\n"); rc = 1; }
        fflush(stdout);
        _exit(rc);
    }
    int status = 0;
    const auto t0 = clock_::now();
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) { std::printf("  [FAIL] %s: waitpid failed\n", c.name); return 1; }
        if (msSince(t0) > c.budgetMs) {
            kill(pid, SIGKILL);
            int st = 0; waitpid(pid, &st, 0);
            std::printf("  [FAIL] %s: TIMEOUT after %d ms — scored RED (a channel that never "
                        "dies, or a thread that never joins, is the bug, not a slow run)\n",
                        c.name, c.budgetMs);
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!WIFEXITED(status)) {
        std::printf("  [FAIL] %s: child did not exit normally (signal %d)\n", c.name,
                    WIFSIGNALED(status) ? WTERMSIG(status) : -1);
        return 1;
    }
    return WEXITSTATUS(status) ? 1 : 0;
}

int main(int argc, char** argv) {
    // Line-buffer: the failure mode is a hang, and a killed process never flushes a block buffer.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const char* filter = (argc > 1) ? argv[1] : nullptr;

    int bad = 0, run = 0;
    for (const Case& c : kCases) {
        if (filter && std::strcmp(filter, c.name) != 0) continue;
        ++run;
        bad += run_case(c);
    }
    if (run == 0) {
        std::printf("receiver-peer-lifetime: NO CASE MATCHED '%s' — refusing to score an empty run\n",
                    filter ? filter : "");
        return 2;
    }
    std::printf("\nreceiver-peer-lifetime: %s (%d/%d cases red)\n", bad ? "FAILURES" : "ALL PASS", bad, run);
    return bad ? 1 : 0;
}
