/*
 * stop-deadlock.cpp — Sender::stop() must not destroy channels while it holds channelsMutex_.
 *
 * The defect (Astra, 2026-10-15): stop() did
 *
 *     { std::lock_guard<std::mutex> lock(channelsMutex_); channels_.clear(); }
 *
 * and ~Channel() joins the receiver thread. That thread is inside listener_->onQualityChanged()
 * whenever a subscriber has just sent <OMTSettings Quality="..."/>, and Sender::onQualityChanged()
 * calls getBestSuggestedQuality(), which takes channelsMutex_. stop() holds the mutex and waits to
 * join a thread that is waiting for the mutex: a self-deadlock with no timer and no way out.
 * removeChannel() and cleanupDisconnected() already unlink under the lock and destroy outside it;
 * stop() was the one path that did not.
 *
 * It is reachable from the UI: PR #15 wired MainActivity::onDestroy -> OMTStreamingManager.stopAll()
 * -> omt_stop() -> Sender::stop(), so the hang lands on the back-button, not in a corner case.
 *
 * How the collision is FORCED rather than sampled (the rule that came out of sw-mutant-video-unlocked
 * staying green on a coin flip):
 *   1. a real subscriber connects to the real accept loop on a real TCP port;
 *   2. it sends the quality XML, and the test's Sender subclass parks the RECEIVER THREAD inside
 *      onQualityChanged() on a gate the test controls;
 *   3. the main thread calls stop() — which cannot reach the receiver, because the receiver is not
 *      in recv(), it is in the callback;
 *   4. the callback is only released once the peer has observed the FIN that ~Channel() sends from
 *      its shutdown(SHUT_RDWR). That FIN is external proof that stop() is already past the point
 *      where it took channelsMutex_, so the pre-fix build is guaranteed to be holding the lock when
 *      the callback resumes and asks for it;
 *   5. pre-fix: the callback blocks on channelsMutex_ forever, ~Channel()'s join never returns, and
 *      stop() never returns. Post-fix: the lock was released before the destruction, the callback
 *      completes, the receiver exits, the join returns.
 *
 * Every case runs in its own forked child with a wall-clock deadline, and the blocking call inside
 * the case runs on a worker thread with its own budget, so a HANG is scored RED — the failure mode
 * of this defect — instead of turning the run into "still running".
 *
 * ASan is on for the whole file (qa/build-host.sh): the released callback reads channel state and
 * walks the channel list while ~Channel() is in progress, which is the use-after-free window a
 * teardown fix can get wrong in the other direction.
 *
 * Build + run on the host (no device, no JVM):  bash qa/build-host.sh
 */
#include "OMTChannel.h"
#include "OMTConstants.h"
#include "OMTProtocol.h"
#include "OMTSender.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>   // quick_exit: leave without unwinding over a thread still inside stop()
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace omt;
using clock_ = std::chrono::steady_clock;

namespace {

int g_fail = 0;

void check(bool cond, const std::string& what) {
    std::printf("    [%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    fflush(stdout);
    if (!cond) ++g_fail;
}

long msSince(clock_::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_::now() - t0).count();
}

std::vector<uint8_t> controlFrame(const std::string& xml) {
    // Exactly what a conforming subscriber puts on the wire: version 1, Metadata, dataLength
    // counting the NUL, metadataLength agreeing with it.
    FrameHeader h{};
    h.version = 1;
    h.frameType = static_cast<uint8_t>(FrameType::Metadata);
    h.timestamp = 0;
    h.metadataLength = static_cast<uint16_t>(xml.size() + 1);
    h.dataLength = static_cast<int32_t>(xml.size() + 1);
    std::vector<uint8_t> out(FrameHeader::SIZE + xml.size() + 1);
    std::memcpy(out.data(), &h, FrameHeader::SIZE);
    memcpy(out.data() + FrameHeader::SIZE, xml.data(), xml.size());
    out.back() = 0;
    return out;
}

bool sendAll(int fd, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    while (n) {
        ssize_t w = ::send(fd, b, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        b += w;
        n -= (size_t)w;
    }
    return true;
}

/// A Sender whose quality callback can be parked by the test. The parking is the whole point: it
/// puts the receiver thread somewhere the main thread can prove it is, instead of hoping a race
/// lands there. The subclass then calls the PRODUCTION onQualityChanged, so the lock acquisition
/// under test is the real one, not a re-implementation of it.
class DeadlockSender : public Sender {
public:
    using Sender::Sender;
    std::atomic<int> entered{0};
    std::atomic<int> permits{0};       // one permit lets exactly one parked callback through
    std::atomic<int> reentrantCalls{0};
    std::atomic<bool> reentrantReturned{false};

    void onQualityChanged(Channel* ch) override {
        ++entered;
        while (permits.load() == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        permits.fetch_sub(1);
        // Re-enter the sender from the callback, the way a UI/JNI callback would: every one of
        // these takes channelsMutex_. This is the "safe against a callback that re-enters" half of
        // the requirement, and in the pre-fix build it is where this thread blocks — stop() is
        // holding the mutex while it waits to join us.
        (void)connectionCount();
        (void)getBestSuggestedQuality();
        (void)getStats();
        ++reentrantCalls;
        reentrantReturned.store(true);
        Sender::onQualityChanged(ch);  // the production path: getBestSuggestedQuality() again
    }
};

SenderConfig qaConfig() {
    SenderConfig c;
    c.name = "qa-stop-deadlock (Camera)";
    c.quality = Quality::Default;   // auto quality: the callback path that takes channelsMutex_
    c.port = 20000 + (getpid() % 19000);   // not 6400: the QA box may have a real listener there
    return c;
}

/// The subscriber: a real client socket to the sender's real listener.
struct Subscriber {
    int fd = -1;
    void connect(unsigned short port) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        if (::connect(fd, (sockaddr*)&a, sizeof(a)) != 0) {
            std::printf("    [FAIL] subscriber could not connect to port %u: %s\n", port, strerror(errno));
            ++g_fail;
            close(fd);
            fd = -1;
            return;
        }
        // Drain the handshake the accept loop writes, so a later MSG_PEEK means FIN and nothing else.
        char buf[512];
        while (::recv(fd, buf, sizeof(buf), MSG_DONTWAIT) > 0) {}
    }
    bool send(const std::string& xml) {
        auto f = controlFrame(xml);
        return sendAll(fd, f.data(), f.size());
    }
    /// Waits for the sender to close its half — the FIN from ~Channel()'s shutdown(). An RST
    /// (ECONNRESET) counts: either way the descriptor is gone, which is the fact being asserted.
    /// Consumes rather than MSG_PEEK on purpose: the accept loop's handshake XML is sitting in this
    /// socket's receive queue, and a peek would report ">0 bytes" forever and never see the EOF.
    bool waitForFin(long budgetMs) {
        char buf[256];
        const auto t = clock_::now();
        while (msSince(t) < budgetMs) {
            ssize_t r = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (r == 0) return true;
            if (r < 0 && errno == ECONNRESET) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
    void stop() { if (fd >= 0) { shutdown(fd, SHUT_RDWR); close(fd); fd = -1; } }
};

/// Run `fn` on a worker and report whether it came back inside the budget. Joining only on success
/// is deliberate: destroying a joinable std::thread calls std::terminate, and this file has already
/// died that way once.
template <typename Fn>
bool callWithin(Fn&& fn, long budgetMs, long* outMs) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    const auto t0 = clock_::now();
    std::thread th([&fn, done] { fn(); done->store(true); });
    while (!done->load() && msSince(t0) < budgetMs)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    *outMs = msSince(t0);
    if (done->load()) { th.join(); return true; }
    th.detach();
    return false;
}

// ------------------------------------------------------------------ the gate

/// stop() while the receiver thread is inside the quality callback. Pre-fix this never returns.
int case_stop_during_quality_callback() {
    static Subscriber sub;              // static: the failure path leaves a thread pointed at it
    SenderConfig cfg = qaConfig();
    // Heap, and deliberately not freed on the failure path: the whole point of the case is that a
    // thread may still be inside sender.stop() when we give up measuring it. A stack object would
    // turn that into a use-after-free of our own making.
    static DeadlockSender* senderP = nullptr;
    if (!senderP) senderP = new DeadlockSender(cfg);
    DeadlockSender& sender = *senderP;
    if (!sender.start()) { check(false, "the sender started its listener socket"); return 1; }

    sub.connect((unsigned short)cfg.port);
    // Wait until the accept loop has actually published the channel: stop() can only deadlock on
    // a channel that is in the vector.
    const auto tReg = clock_::now();
    while (sender.connectionCount() == 0 && msSince(tReg) < 2000)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(sender.connectionCount() == 1, "one subscriber is registered in Sender::channels_");

    // The XML that puts the receiver thread into the callback.
    check(sub.send("<OMTSettings Quality=\"High\" />"), "subscriber sent the OMTSettings quality XML");
    const auto tPark = clock_::now();
    while (sender.entered.load() == 0 && msSince(tPark) < 2000)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(sender.entered.load() == 1,
          "the RECEIVER THREAD is parked inside Sender::onQualityChanged() (no permit issued)");

    // Main thread tears down while that thread is parked in the callback.
    auto stopStarted = std::make_shared<std::atomic<bool>>(false);
    auto stopReturned = std::make_shared<std::atomic<bool>>(false);
    std::thread mainThread([&sender, stopStarted, stopReturned] {
        stopStarted->store(true);
        sender.stop();
        stopReturned->store(true);
    });

    // Release the callback only once ~Channel() has proven it is running, by observing its FIN: that
    // is the ordering that makes the pre-fix build hold channelsMutex_ when the callback resumes.
    auto finSeen = std::make_shared<std::atomic<bool>>(false);
    std::thread releaser([&] {
        const auto t = clock_::now();
        while (!stopStarted->load() && msSince(t) < 1000) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        *finSeen = sub.waitForFin(3000);
        sender.permits.store(1);
    });

    long stopMs = -1;
    const auto tStop = clock_::now();
    while (!stopReturned->load() && msSince(tStop) < 4000) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    stopMs = msSince(tStop);
    releaser.join();

    const bool returned = stopReturned->load();
    if (returned) mainThread.join();   // it came back: reap it normally
    else mainThread.detach();          // it IS the deadlock; the child exits out from under it

    std::printf("      stop() returned %s after %ld ms; teardown FIN observed=%s; "
                "re-entrant sender calls completed=%d\n",
                returned ? "yes" : "NO", stopMs, finSeen->load() ? "yes" : "NO",
                sender.reentrantCalls.load());

    // The precondition, asserted rather than assumed: without the FIN the case measured nothing.
    check(finSeen->load(),
          "stop() got as far as ~Channel()'s shutdown() while the callback was parked "
          "(otherwise the collision was never forced)");
    check(returned, "stop() returned within 4 s — no self-deadlock between the join and the quality "
                    "callback's channelsMutex_");
    check(sender.reentrantReturned.load(),
          "the callback re-entered the sender (connectionCount/getBestSuggestedQuality/getStats) and "
          "all three returned — the stop path tolerates a re-entrant callback");
    if (returned)
        check(sender.connectionCount() == 0, "the channel list is empty after stop()");
    else
        std::printf("    [skip] connectionCount() would only block behind the wedged stop()\n");

    sub.stop();
    if (!returned) {
        // The hang is the verdict. Returning would let this function's stack (and the sender on it)
        // go away under a thread still inside sender.stop(), so leave by the side door.
        fflush(stdout);
        std::quick_exit(1);
    }
    delete senderP;          // ~Sender() -> stop() (now a no-op) + destroyEncoder(), under ASan
    senderP = nullptr;
    return g_fail ? 1 : 0;
}

/// Ordinary teardown, with a subscriber that is behaving: stop() must still be prompt and complete.
/// This one is a regression guard for the fix, not a gate that is expected to be red pre-fix.
int case_stop_plain_teardown() {
    static Subscriber sub;
    SenderConfig cfg = qaConfig();
    DeadlockSender sender(cfg);
    if (!sender.start()) { check(false, "the sender started its listener socket"); return 1; }
    sub.connect((unsigned short)cfg.port);
    const auto tReg = clock_::now();
    while (sender.connectionCount() == 0 && msSince(tReg) < 2000)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(sender.connectionCount() == 1, "plain: one subscriber is registered");

    long stopMs = -1;
    bool returned = callWithin([&] { sender.stop(); }, 2000, &stopMs);
    std::printf("      plain: stop() returned %s after %ld ms\n", returned ? "yes" : "NO", stopMs);
    check(returned, "plain: stop() on an idle subscriber returns within 2 s");
    check(sender.connectionCount() == 0, "plain: every channel was destroyed");
    check(sub.waitForFin(500), "plain: the subscriber saw the connection close");

    // ~Sender() re-enters stop(); the second call must be a no-op, not a second join.
    long againMs = -1;
    returned = callWithin([&] { sender.stop(); }, 1000, &againMs);
    check(returned, "plain: stop() is idempotent — a second call returns immediately");
    sub.stop();
    return g_fail ? 1 : 0;
}

/// Two threads race stop() (UI onDestroy vs JNI cleanup — the process-global g_sender makes this
/// real), and a callback-driven re-entrant stop() must not be able to wedge either of them.
int case_stop_concurrent_callers() {
    static Subscriber sub;
    SenderConfig cfg = qaConfig();
    DeadlockSender sender(cfg);
    if (!sender.start()) { check(false, "the sender started its listener socket"); return 1; }
    sub.connect((unsigned short)cfg.port);
    const auto tReg = clock_::now();
    while (sender.connectionCount() == 0 && msSince(tReg) < 2000)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    std::atomic<bool> go{false};
    std::atomic<int> returnedCount{0};
    std::vector<std::thread> callers;
    for (int i = 0; i < 3; ++i)
        callers.emplace_back([&] { while (!go.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                   sender.stop(); ++returnedCount; });
    go.store(true);

    const auto t0 = clock_::now();
    while (returnedCount.load() < 3 && msSince(t0) < 3000) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    long allMs = msSince(t0);
    std::printf("      %d/3 concurrent stop() callers returned after %ld ms\n", returnedCount.load(), allMs);
    for (auto& th : callers) { if (returnedCount.load() == 3) th.join(); else th.detach(); }
    check(returnedCount.load() == 3, "concurrent: three threads racing stop() all returned — one "
                                     "owner does the teardown, the rest do not double-join");
    if (returnedCount.load() == 3)
        check(sender.connectionCount() == 0, "concurrent: the channel list ended up empty");
    else
        std::printf("    [skip] connectionCount() would only block behind a wedged stop()\n");
    sub.stop();
    if (returnedCount.load() != 3) { fflush(stdout); std::quick_exit(1); }  // detached caller is inside stop()
    return g_fail ? 1 : 0;
}

// ------------------------------------------------------------------ runner

struct Case {
    const char* name;
    int (*fn)();
};

constexpr long kCaseTimeoutMs = 20000;

/// fork per case so a hang is a RED verdict with a printed cause, not a run that never finishes.
int run_case(const Case& c) {
    std::printf("  == %s\n", c.name);
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) { check(false, "fork() failed"); return 1; }
    if (pid == 0) {
        int rc = c.fn();
        fflush(stdout);
        // exit() on the clean path so ASan's LeakSanitizer teardown checks actually run; _exit()
        // when the case already failed, because a wedged thread must not be able to eat the deadline.
        if (rc == 0) exit(0);
        _exit(rc);
    }
    int status = 0;
    const auto t0 = clock_::now();
    while (true) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) { check(false, "waitpid failed"); return 1; }
        if (msSince(t0) > kCaseTimeoutMs) {
            kill(pid, SIGKILL);
            int st = 0;
            waitpid(pid, &st, 0);
            std::printf("    [FAIL] %s: TIMEOUT after %ld ms — the case never returned, which IS the "
                        "deadlock this test exists to catch. Scored RED, not 'still running'.\n",
                        c.name, (long)kCaseTimeoutMs);
            fflush(stdout);
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        std::printf("    PASS %s\n", c.name);
        return 0;
    }
    if (WIFSIGNALED(status))
        std::printf("    FAIL %s: child died on signal %d (an ASan report above the verdict is the "
                    "evidence)\n", c.name, WTERMSIG(status));
    else
        std::printf("    FAIL %s: %d assertion(s) red\n", c.name, WEXITSTATUS(status));
    fflush(stdout);
    return 1;
}

const Case kCases[] = {
    {"stop-during-quality-callback", case_stop_during_quality_callback},
    {"stop-plain-teardown", case_stop_plain_teardown},
    {"stop-concurrent-callers", case_stop_concurrent_callers},
};

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);   // the failure mode is a hang: never block-buffer the log
    const char* filter = (argc > 1) ? argv[1] : nullptr;
    int bad = 0, run = 0;
    for (const Case& c : kCases) {
        if (filter && std::strcmp(filter, c.name) != 0) continue;
        ++run;
        bad += run_case(c);
    }
    if (run == 0) {
        std::printf("stop-deadlock: NO CASE MATCHED '%s' — refusing to score an empty run\n",
                    filter ? filter : "");
        return 2;
    }
    std::printf("\nstop-deadlock: %s (%d/%d cases red)\n", bad ? "FAILURES" : "ALL PASS", bad, run);
    return bad ? 1 : 0;
}
