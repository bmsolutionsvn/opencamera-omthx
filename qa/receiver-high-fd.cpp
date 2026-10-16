/*
 * receiver-high-fd.cpp — the receiver thread must not index a stack-resident fd_set with the
 * subscriber's own descriptor number.
 *
 * The defect (qa-ompin/FOREIGN-IN-DIAGNOSTICS-20261015.md, scenarios C0 and C1, measured against
 * this file at 3d64d14a): receiverLoop() waited for the peer with
 *
 *     fd_set readfds;
 *     FD_ZERO(&readfds);
 *     FD_SET(socketFd_, &readfds);            // socketFd_ is the ACCEPTED descriptor
 *     select(socketFd_ + 1, &readfds, nullptr, nullptr, &tv);
 *
 * `fd_set` is a stack array sized for FD_SETSIZE (1024) descriptors and FD_SET performs no bounds
 * check at all. An accepted subscriber at fd >= 1024 — routine on a process that has been up for a
 * day, and exactly what a sender holding the camera, the encoder, the DNS-SD sockets and its log
 * files produces — wrote outside that array. Measured under ASan, immediately after the accept:
 *
 *     ERROR: AddressSanitizer: stack-buffer-overflow
 *     READ of size 8 ... 'readfds' <== Memory access at offset 200 overflows this variable
 *     [ACCEPT fd=1104]
 *
 * and the consequence on the wire was worse than the report: the subscription was never processed.
 * C0 (select, fd 1104) delivered 0 frames with the socket open; C1 — the same peer at the same
 * descriptor number, poll() in that loop — delivered 179/179. Nothing else about the two runs
 * differed, so the descriptor ceiling is the whole cause.
 *
 * WHAT MAKES THIS A GATE AND NOT A DEMONSTRATION
 *   * the descriptor really landed at or past FD_SETSIZE — asserted, not printed. A run that failed
 *     to push the fd high would pass trivially and prove nothing, so it is scored red instead;
 *   * the peer's subscribe frame registers AND the frames arrive on its side of the wire. Under a
 *     select() build the out-of-bounds write either dies in ASan or leaves the loop waiting on a
 *     bit that was never in the mask, and both are red here — the functional half is what still
 *     catches the defect on a build without sanitizers;
 *   * the identical exchange at an ordinary descriptor still works, so the poll() path cannot be
 *     green simply because nothing was ever asked of it.
 *
 * What is NOT the gate: the functional assertions alone. Run without a sanitizer, the out-of-bounds
 * write lands wherever the compiler happened to put something, and the same build passed one case
 * and produced the C0 signature (0 frames, 81 bytes, channel dead) in the other. Undefined behaviour
 * that is only sometimes visible is not a test — ASan is what makes this red every run, which is why
 * qa/build-host.sh exports ASAN_SYMBOLIZER_PATH and why the report, quoted in the PR, is the
 * evidence rather than a summary line that says FAIL.
 *
 * Build + run on the host (no device, no JVM):  bash qa/build-host.sh
 * The pre-fix run and the `hp-mutant-select` mutant must both be red; the ASan report in the child's
 * output is the evidence, which is why the runner exports ASAN_SYMBOLIZER_PATH.
 */
#include "OMTChannel.h"
#include "OMTConstants.h"
#include "OMTProtocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/select.h>   // FD_SETSIZE only: this case asserts against the ceiling select() has
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace omt;
using clock_ = std::chrono::steady_clock;

static long msSince(clock_::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_::now() - t0).count();
}

static int g_fail = 0;
static void check(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fail;
}

// ------------------------------------------------------------------------------------------------
// Descriptor squatting. open() returns the LOWEST free descriptor, so occupying every number from 3
// upward is what pushes every later socket() past FD_SETSIZE; there is no way to ask for a specific
// number. /dev/null keeps the kernel side of each squatted descriptor trivial.
// ------------------------------------------------------------------------------------------------
static std::vector<int> g_held;

static bool squatPast(int ceiling) {
    for (;;) {
        int fd = ::open("/dev/null", O_RDONLY);
        if (fd < 0) {
            std::printf("      open(/dev/null) failed after %zu descriptors: %s — raise the "
                        "NOFILE limit\n", g_held.size(), strerror(errno));
            return false;
        }
        g_held.push_back(fd);
        if (fd >= ceiling) return true;
        if (g_held.size() > 40000) return false;      // the rlimit is not what we need
    }
}

static void releaseHeld() {
    for (int fd : g_held) ::close(fd);
    g_held.clear();
}

/// The subscriber side: reads everything, counts complete OMT records.
struct Peer {
    int client = -1;
    std::atomic<bool> stop{false};
    std::atomic<int> videoFrames{0};
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
            uint8_t chunk[64 * 1024];
            while (!stop.load()) {
                struct pollfd p{client, POLLIN, 0};
                int pr = ::poll(&p, 1, 20);
                if (pr < 0) { if (errno == EINTR) continue; break; }
                if (pr == 0) continue;
                ssize_t n = syscall(SYS_recvfrom, client, chunk, sizeof chunk, 0, nullptr, nullptr);
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                if (n <= 0) break;
                bytes += (long long)n;
                buf.insert(buf.end(), chunk, chunk + n);
                size_t pos = 0;
                while (buf.size() - pos >= FrameHeader::SIZE) {
                    FrameHeader h;
                    memcpy(&h, buf.data() + pos, FrameHeader::SIZE);
                    if (h.version != 1 || h.dataLength < 0) return;   // hostile: stop counting
                    const size_t total = FrameHeader::SIZE + (size_t)h.dataLength;
                    if (buf.size() - pos < total) break;
                    if (h.frameType == 2) ++videoFrames;
                    pos += total;
                }
                if (pos) buf.erase(buf.begin(), buf.begin() + pos);
            }
        });
    }

    /// One control frame in the sender's own dialect (NUL-terminated, metadataLength == len+1).
    bool sendMeta(const char* xml) {
        const size_t len = strlen(xml);
        FrameHeader h{};
        h.version = 1; h.frameType = 1; h.timestamp = 0;
        h.metadataLength = (uint16_t)(len + 1);
        h.dataLength = (int32_t)(len + 1);
        std::vector<uint8_t> out(sizeof(FrameHeader) + len + 1);
        memcpy(out.data(), &h, sizeof(FrameHeader));
        memcpy(out.data() + sizeof(FrameHeader), xml, len);
        out[sizeof(FrameHeader) + len] = 0;
        size_t off = 0;
        while (off < out.size()) {
            ssize_t w = ::send(client, out.data() + off, out.size() - off, MSG_NOSIGNAL);
            if (w <= 0) return false;
            off += (size_t)w;
        }
        return true;
    }

    void finish() {
        stop = true;
        if (reader.joinable()) reader.join();
        if (client >= 0) { ::shutdown(client, SHUT_RDWR); ::close(client); client = -1; }
    }
};

struct CountingListener : public IChannelListener {
    std::atomic<int> disconnected{0};
    void onDisconnected(Channel*) override { ++disconnected; }
    void onTallyChanged(Channel*) override {}
    void onQualityChanged(Channel*) override {}
};

/// One whole exchange on one subscriber socket: subscribe, register, stream, count on the wire.
/// Returns the process's failure count so the caller can score the case.
static int runExchange(const char* label, int squatCeiling, bool expectHighFd) {
    g_fail = 0;
    std::printf("\ncase %s (%s)\n", label,
                expectHighFd ? "subscriber socket forced past FD_SETSIZE" : "ordinary descriptor");

    if (squatCeiling > 0 && !squatPast(squatCeiling)) {
        check(false, "the descriptors could not be squatted high enough — the case cannot be run, "
                     "which is red, not skipped");
        return 1;
    }

    int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) { check(false, "socket() for the listener failed"); return 1; }
    int one = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    bind(listenFd, (sockaddr*)&a, sizeof(a));
    socklen_t sl = sizeof(a);
    getsockname(listenFd, (sockaddr*)&a, &sl);
    listen(listenFd, 8);

    Peer peer;
    peer.client = Peer::connectTo(ntohs(a.sin_port));
    if (peer.client < 0) { check(false, "the subscriber could not connect"); ::close(listenFd); return 1; }
    int accepted = ::accept(listenFd, nullptr, nullptr);
    if (accepted < 0) { check(false, "accept() failed"); ::close(listenFd); return 1; }
    sockaddr_in peerAddr{}; socklen_t pl = sizeof(peerAddr);
    getpeername(accepted, (sockaddr*)&peerAddr, &pl);

    std::printf("      FD_SETSIZE=%d; listener fd=%d, subscriber fd=%d, CHANNEL fd=%d\n",
                (int)FD_SETSIZE, listenFd, peer.client, accepted);
    const bool high = accepted >= (int)FD_SETSIZE;
    if (expectHighFd) {
        // The anti-vacuity condition: FD_SET cannot corrupt anything below its own ceiling, so a
        // run that failed to push the descriptor over the line has not tested the defect at all.
        check(high, "precondition: the channel's descriptor is >= FD_SETSIZE (otherwise FD_SET is "
                    "in bounds and this run proves nothing)");
        if (!high) { peer.finish(); ::close(accepted); ::close(listenFd); releaseHeld(); return 1; }
    } else {
        check(!high, "precondition: this control really is the ordinary-descriptor case");
    }

    peer.startReading();
    CountingListener listener;
    Channel* ch = new Channel(accepted, peerAddr, Constants::NETWORK_SEND_BUFFER, &listener);

    // The greeting the accept loop writes, so the peer has something to parse before it subscribes.
    ch->sendMetadataSync("<OMTInfo ProductName=\"QA\" Manufacturer=\"Hermes\" Version=\"1.0\" />");

    check(peer.sendMeta(MetadataConstants::CHANNEL_SUBSCRIBE_VIDEO), "the subscribe frame was sent");
    const auto t0 = clock_::now();
    while (!ch->isVideoSubscribed() && ch->isConnected() && msSince(t0) < 3000)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::printf("      subscribed=%s connected=%s after %ld ms (%lld bytes read by the channel)\n",
                ch->isVideoSubscribed() ? "yes" : "NO", ch->isConnected() ? "yes" : "NO",
                msSince(t0), (long long)ch->getStats().bytesReceived);
    // This is the C0 symptom: the accept happened, the bytes were there, and the subscription never
    // registered because the readiness test consulted a bit FD_SET had written off the end of the
    // stack array. Asserted BEFORE any frame is pushed, so a select() build cannot pass by accident.
    check(ch->isVideoSubscribed(),
          expectHighFd
              ? "the subscription registered on a descriptor >= FD_SETSIZE (pre-fix: the ACCEPT was "
                "logged and the subscribe was never processed)"
              : "the subscription registered on an ordinary descriptor (the poll() path is not green "
                "simply because nothing was asked of it)");
    check(ch->isConnected(), "and the channel was not killed by the read");

    // ...then the part a viewer actually notices: does the video arrive.
    FrameHeader hdr{};
    hdr.version = 1; hdr.frameType = 2; hdr.timestamp = 0; hdr.metadataLength = 0;
    const size_t payloadSize = 256 * 1024;
    hdr.dataLength = (int32_t)(sizeof(VideoExtHeader) + payloadSize);
    VideoExtHeader ext{};
    ext.codec = Codec::VMX1; ext.width = 512; ext.height = 512;
    ext.frameRateN = 30; ext.frameRateD = 1; ext.aspectRatio = 1.0f; ext.flags = 0; ext.colorSpace = 709;
    std::vector<uint8_t> payload(payloadSize);
    uint32_t x = 12345u;
    for (size_t i = 0; i < payloadSize; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; payload[i] = (uint8_t)(x >> 24); }

    const int want = 10;
    for (int i = 0; i < want; ++i) {
        const auto t1 = clock_::now();
        ch->sendAsync(&hdr, sizeof(hdr), &ext, sizeof(ext), payload.data(), payload.size());
        while (ch->framesSent() < i + 1 && ch->isConnected() && msSince(t1) < 2000)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto t2 = clock_::now();
    while (peer.videoFrames.load() < want && msSince(t2) < 3000)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::printf("      sender wrote %lld frames / %lld bytes; subscriber read %d video records / %lld bytes\n",
                (long long)ch->framesSent(), (long long)ch->getStats().bytesSent,
                peer.videoFrames.load(), (long long)peer.bytes.load());
    check(ch->framesSent() == want, "every frame left the channel");
    check(peer.videoFrames.load() == want,
          "the subscriber RECEIVED every frame (pre-fix at fd>=FD_SETSIZE: 0 frames, socket open)");

    ch->disconnect();
    delete ch;
    peer.finish();
    ::close(listenFd);
    releaseHeld();
    return g_fail ? 1 : 0;
}

// ------------------------------------------------------------------------------------------------
// runner: one forked child per case, RED if it does not exit inside the deadline
// ------------------------------------------------------------------------------------------------
struct Case { const char* name; int (*fn)(); int budgetMs; };

static int case_high_fd()      { return runExchange("high-fd-subscriber",        FD_SETSIZE, true); }
static int case_much_higher()  { return runExchange("much-higher-fd-subscriber", 2000,        true); }
static int case_ordinary_fd()  { return runExchange("ordinary-fd-control",          0,        false); }

static const Case kCases[] = {
    {"high-fd-subscriber",        case_high_fd,        30000},
    {"much-higher-fd-subscriber", case_much_higher,    45000},
    {"ordinary-fd-control",       case_ordinary_fd,    30000},
};

static int run_case(const Case& c) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) { std::perror("fork"); return 1; }
    if (pid == 0) {
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
            std::printf("  [FAIL] %s: TIMEOUT after %d ms — scored RED (a receiver thread that never "
                        "sees its subscriber is the bug, not a slow run)\n", c.name, c.budgetMs);
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!WIFEXITED(status)) {
        std::printf("  [FAIL] %s: child did not exit normally (signal %d) — an ASan abort on the "
                    "stack-buffer-overflow lands here\n", c.name,
                    WIFSIGNALED(status) ? WTERMSIG(status) : -1);
        return 1;
    }
    if (WEXITSTATUS(status)) {
        std::printf("  [FAIL] %s: %d assertion(s) red\n", c.name, WEXITSTATUS(status));
        return 1;
    }
    std::printf("  [PASS] %s\n", c.name);
    return 0;
}

int main(int argc, char** argv) {
    // Line-buffer: the failure mode is a hang or an ASan abort, and a killed process never flushes
    // a block buffer — the run log would come back empty and the evidence would be gone.
    setvbuf(stdout, nullptr, _IOLBF, 0);

    // The high-fd cases need ~2100 descriptors; the default soft limit on this box is 4096, but a
    // CI container may be lower. Raise it to the hard limit (bounded) rather than skipping the case.
    struct rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rlim_t want = std::min<rlim_t>(rl.rlim_max, 8192);
        if (rl.rlim_cur < want) {
            rl.rlim_cur = want;
            if (setrlimit(RLIMIT_NOFILE, &rl) != 0) std::perror("setrlimit(RLIMIT_NOFILE)");
        }
        getrlimit(RLIMIT_NOFILE, &rl);
        std::printf("RLIMIT_NOFILE: cur=%llu max=%llu\n",
                    (unsigned long long)rl.rlim_cur, (unsigned long long)rl.rlim_max);
    }

    const char* filter = (argc > 1) ? argv[1] : nullptr;
    int bad = 0, run = 0;
    for (const Case& c : kCases) {
        if (filter && std::strcmp(filter, c.name) != 0) continue;
        ++run;
        bad += run_case(c);
    }
    if (run == 0) {
        std::printf("receiver-high-fd: NO CASE MATCHED '%s' — refusing to score an empty run\n",
                    filter ? filter : "");
        return 2;
    }
    std::printf("\nreceiver-high-fd: %s (%d/%d cases red)\n", bad ? "FAILURES" : "ALL PASS", bad, run);
    return bad ? 1 : 0;
}
