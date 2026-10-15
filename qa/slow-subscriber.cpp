/*
 * slow-subscriber.cpp — proves a viewer that stops draining its TCP window cannot take the
 * stream down with it.
 *
 * Why this exists: Channel used to hand a frame to sendAllBlocking(), which cleared O_NONBLOCK
 * and called bare send() until every byte left. A subscriber on a flaky Wi-Fi link (or one that
 * vanished without sending RST) fills its receive window, that call never returns, and the four
 * AsyncPool slots it owns stay acquired. connected_ is only cleared on a send *error*, which the
 * blocking path never reaches, so the dead channel is never pruned either: the phone keeps
 * encoding into a black hole and still counts that viewer as connected.
 *
 * The peer below is exactly that subscriber: it accepts the connection, shrinks its own receive
 * buffer so the window closes after a couple of frames, and then never calls read().
 *
 * Build + run on the host (no device, no JVM):  bash qa/build-host.sh
 * Expected: ALL PASS on the fixed code. Against the pre-fix sources the same binary must report
 * G1 and G2 as failures — that A/B is what proves the test has teeth (see qa/build-host.sh).
 */
#include "OMTChannel.h"
#include "OMTConstants.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace omt;
using clock_ = std::chrono::steady_clock;

static long msSince(clock_::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_::now() - t0).count();
}

// A subscriber that connects and never reads. SO_RCVBUF is set before any frame is sent so the
// advertised window closes almost immediately instead of after a megabyte of loopback buffering.
struct StalledPeer {
    int listenFd = -1;
    int accepted = -1;
    unsigned short port = 0;
    std::atomic<bool> ready{false};
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
            if (accepted < 0) { ready = true; return; }
            int rcv = 8192;
            setsockopt(accepted, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
            ready = true;
            // ...and never read. This line is the entire point of the file.
            while (accepted >= 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        });
        // NB: do NOT wait for the accept here — nothing connects until start() returns.
    }

    // Call after the client has connected.
    void waitAccepted() { while (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    void stop() {
        if (accepted >= 0) { shutdown(accepted, SHUT_RDWR); close(accepted); accepted = -1; }
        if (listenFd >= 0) { close(listenFd); listenFd = -1; }
        if (th.joinable()) th.join();
    }
};

static int connectClient(unsigned short port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::connect(fd, (sockaddr*)&a, sizeof(a)) != 0) { ::perror("connect"); return -1; }
    return fd;
}

int main() {
    // Line-buffer the log: this test's failure mode is a HANG, and a killed process never
    // flushes a block buffer — the run log comes back empty and the hang is undiagnosable.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    int fail = 0;
    auto ok = [&](bool cond, const char* what) {
        std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
        if (!cond) ++fail;
    };

    StalledPeer peer;
    peer.start();

    int fd = connectClient(peer.port);
    peer.waitAccepted();
    ok(fd >= 0, "connected a client socket to the stalled peer");

    sockaddr_in me{};
    socklen_t ml = sizeof(me);
    getsockname(fd, (sockaddr*)&me, &ml);

    Channel* ch = new Channel(fd, me, Constants::NETWORK_SEND_BUFFER, nullptr);
    ok(ch->isConnected(), "channel starts connected");

    // ---- fill the window: 16 frames of 512 KB through the production path ------------------
    // Each sendAsync() is a queue hand-off; it must stay cheap no matter what the peer is doing,
    // because it runs on the thread that produces frames.
    std::vector<uint8_t> payload(512 * 1024, 0xAB);
    FrameHeader hdr{};
    hdr.version = 1;
    hdr.frameType = 1;
    VideoExtHeader ext{};
    ext.width = 1280; ext.height = 720; ext.aspectRatio = 1.7778f; ext.colorSpace = 709;

    long worstProducerMs = 0;
    int acceptedFrames = 0;
    for (int i = 0; i < 16; ++i) {
        auto t0 = clock_::now();
        int r = ch->sendAsync(&hdr, sizeof(hdr), &ext, sizeof(ext), payload.data(), payload.size());
        long ms = msSince(t0);
        if (ms > worstProducerMs) worstProducerMs = ms;
        if (r > 0) ++acceptedFrames;
    }
    std::printf("      frames accepted by the pool: %d/16, slowest producer call %ld ms\n",
                acceptedFrames, worstProducerMs);
    ok(worstProducerMs < 50, "the producer was never blocked by the stalled subscriber");
    ok(acceptedFrames > 0 && acceptedFrames < 16,
       "the bounded pool dropped the overflow instead of growing without limit");

    // ---- G1: the wedged subscriber must be EVICTED, not waited on ---------------------------
    // Before the fix this never happened: sendAllBlocking sat in send() forever, and because
    // connected_ is only cleared on a send error the pruning pass never saw the channel as dead.
    auto tEv = clock_::now();
    auto deadline = tEv + std::chrono::milliseconds(3000);
    while (ch->isConnected() && clock_::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    bool evicted = !ch->isConnected();
    std::printf("      eviction %s after %ld ms (send deadline is %d ms)\n",
                evicted ? "took" : "NEVER HAPPENED, waited", msSince(tEv),
                Constants::NETWORK_SEND_TIMEOUT_MS);
    ok(evicted, "G1: a subscriber that stops draining is declared disconnected within ~1s");

    // ---- G2: metadata must not block the caller forever -------------------------------------
    // sendMetadataSync used to clear O_NONBLOCK and issue two untimed send()s; its only caller
    // held the sender-wide mutex, so one wedged viewer froze the camera thread.
    auto t0 = clock_::now();
    std::atomic<bool> returned{false};
    std::atomic<bool> result{false};
    std::thread caller([&] { result = ch->sendMetadataSync("<Omtdescription version=\"1.0\"/>"); returned = true; });
    while (!returned && msSince(t0) < 2000) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    long metaMs = msSince(t0);
    std::printf("      sendMetadataSync returned %s after %ld ms\n",
                returned.load() ? "normally" : "NOT — it is still inside send()", metaMs);
    ok(returned.load(), "G2: sendMetadataSync is bounded — it cannot hang the caller");
    ok(!result.load(), "G2b: it reported failure for the wedged peer rather than pretending success");
    // Join only if it came back. Destroying a joinable thread calls std::terminate (this test
    // aborted exactly that way), and joining a wedged one would hang the very thing we measure,
    // so the failure path detaches and lets process exit reap it.
    if (returned.load()) caller.join(); else caller.detach();

    // ---- G3: teardown must complete; a stuck thread is the 'reboot the phone' symptom -------
    auto t1 = clock_::now();
    delete ch;
    long dtorMs = msSince(t1);
    std::printf("      ~Channel completed in %ld ms\n", dtorMs);
    ok(dtorMs < 3000, "G3: destroying the channel does not hang on a wedged sender thread");

    peer.stop();
    std::printf("\nslow-subscriber: %s (%d failures)\n", fail ? "FAILURES" : "ALL PASS", fail);
    return fail ? 1 : 0;
}
