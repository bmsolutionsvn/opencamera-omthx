/*
 * thread-task-join.cpp — proves ThreadTask::Join() reports completion only when the queued work
 * has actually run, and that tearing the pool down cannot lose its wakeup.
 * (P0 #11 Join half, ported from the desktop vendored copy at
 *  nodos-yded-linux/plugins/yd.net.ompout/vendor/libvmx/src/thread_tasks.h.)
 *
 * Why this exists: Join() was
 *
 *     std::unique_lock<std::mutex> lock(mtx);
 *     if (queue.size() > 0) complete.wait(lock);
 *
 * A bare condvar wait. std::condition_variable::wait is explicitly permitted to return without any
 * notify — a spurious wake is a documented behaviour, not a theoretical one — and when it does,
 * Join() has just told its caller "the slice work is finished" with tasks still sitting in the
 * queue. Join()'s callers are VMX_Decode (vmxcodec.cpp:930) and VMX_Encode (vmxcodec.cpp:1708),
 * both of which return straight into the slice streams; VMX_Destroy, and the resolution-change
 * path in VMX_SetDimensions that recreates the pool, then free what a worker is still writing.
 * That is a use-after-free whose only trigger is a wake that nobody schedules.
 *
 * TaskLoop() compounded it: `while (running)` read the flag OUTSIDE the mutex while Destroy()
 * wrote it under the mutex, so Destroy could clear running and fire its notify_all() in the window
 * between that read and the worker's cv.wait() — the worker then slept through the only wakeup it
 * was ever going to get and thread.join() blocked forever.
 *
 * The fix is the desktop one: predicate-guarded waits — complete.wait(lock, []{return queue.empty();})
 * and cv.wait(lock, []{return !running || !queue.empty();}) — with `running` read only under mtx.
 *
 * A spurious wake cannot be ordered from the C++ library, so J1 injects the exact event the
 * predicate defends against: a notify_all() on `complete` that does not mean "the queue is empty".
 * A bare wait cannot tell that apart from the real thing; a predicate wait can.
 *
 * Build + run on the host (no device, no JVM):  bash qa/build-host.sh
 * Expected: ALL PASS on the fixed code. With the fix reverted on a copy of the tree the same
 * binary must go red — and the reverted copy keeps the `delete[] tasks->tasks` line that landed in
 * df66b01f, so this scenario tests the Join half and nothing else.
 */
#include "thread_tasks.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>

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

// A watchdog, because the failure mode under test is a HANG: if any scenario below stops making
// progress the harness would sit in `timeout 90` and the run log would come back empty.
// static storage: the watchdog thread is detached, so its flag must outlive every scope.
std::atomic<bool> doneJ1{false}, doneJ2{false}, doneJ3{false};

void watchdog(const char* stage, int seconds, std::atomic<bool>& scenarioDone) {
    std::thread([stage, seconds, &scenarioDone] {
        for (int i = 0; i < seconds * 10; ++i) {
            if (scenarioDone.load()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::printf("  [FAIL] HUNG in %s — the thread pool did not make progress within %ds\n",
                    stage, seconds);
        std::fflush(stdout);
        std::_Exit(1);
    }).detach();
}

// ---------------------------------------------------------------- J1: the injected spurious wake
// The worker thread is deliberately NOT started: it would race the queue and turn a proof into a
// coin flip. Instead the scenario plays the worker itself, in the order TaskLoop would, and fires
// the wake Join must be able to ignore.
void test_join_survives_a_spurious_wake() {
    ThreadTask t;                       // no Initialize(): nothing consumes the queue but us
    std::atomic<int> ran{0};
    {
        std::lock_guard<std::mutex> lk(t.mtx);
        t.queue.push([&ran] {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));   // real work, real duration
            ++ran;
        });
    }

    std::atomic<bool> drained{false};
    std::thread fakeWorker([&t, &ran, &drained] {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        // The spurious wake. Identical, from inside wait(), to the one libc is allowed to hand out
        // for no reason at all: a notify on `complete` while the queue is still full.
        t.complete.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        std::function<void()> f;
        {
            std::lock_guard<std::mutex> lk(t.mtx);
            if (!t.queue.empty()) { f = std::move(t.queue.front()); t.queue.pop(); }
        }
        if (f) f();                                   // TaskLoop runs tasks with mtx held; the
        drained = true;                               // queue is empty again only after this point
        t.complete.notify_all();                      // ...which is what `complete` means
    });

    auto t0 = clock_::now();
    t.Join();
    // Snapshot BEFORE the fake worker is joined: what matters is the state of the world at the
    // instant Join() claimed the work was finished. Reading these afterwards would let the worker
    // catch up and hide the bug — which is exactly how this scenario first scored the pre-fix
    // build as a pass.
    const long waited = msSince(t0);
    const int ranAtReturn = ran.load();
    const bool drainedAtReturn = drained.load();
    fakeWorker.join();

    std::printf("      J1: Join() returned after %ld ms with %d task(s) executed, queue drained=%s "
                "(the injected wake arrived at ~120 ms, the task needs ~180 ms)\n",
                waited, ranAtReturn, drainedAtReturn ? "yes" : "NO");
    ok(ranAtReturn == 1,
       "J1: Join() did not report completion while queued work was still pending");
    ok(drainedAtReturn, "J1b: the queue really was drained before Join() returned");
}

// ---------------------------------------------------------- J2: Join must wait for real workers ---
// The same contract with the production TaskLoop driving the queue: 200 tasks, one Join. Guards
// the other direction — a predicate wait that returned early would under-report, and one that
// never returned would hang the encoder.
void test_join_waits_for_every_task() {
    const int kTasks = 200;
    for (int round = 0; round < 20; ++round) {
        ThreadTask t;
        t.Initialize();
        std::atomic<int> counter{0};
        for (int i = 0; i < kTasks; ++i)
            t.Push([&counter] { std::this_thread::sleep_for(std::chrono::microseconds(20)); ++counter; });
        t.Join();
        if (counter.load() != kTasks) {
            std::printf("      J2: round %d — Join() returned with %d/%d tasks executed\n",
                        round, counter.load(), kTasks);
            ok(false, "J2: Join() waited for every queued task");
            t.Destroy();
            return;
        }
        t.Destroy();
    }
    ok(true, "J2: 20 rounds x 200 tasks — Join() returned only with the queue drained");
}

// ------------------------------------------- J3: Destroy must not be able to lose its wakeup ---
// `while (running)` outside the mutex is the lost-wakeup half: Destroy clears running and notifies
// in the window before the worker reaches cv.wait, and thread.join() then blocks forever. That is
// the VMX_SetDimensions / VMX_Destroy path (a resolution change mid-stream recreates the pool).
// It is a race, so the scenario runs it thousands of times rather than once.
void test_destroy_never_hangs() {
    const int kRounds = 3000;
    std::atomic<int> completed{0};
    std::atomic<bool> finished{false};
    std::thread hammer([&completed, &finished] {
        for (int r = 0; r < kRounds; ++r) {
            ThreadTasks* th = CreateTasks(2);
            for (int i = 0; i < th->numThreads; ++i)
                th->tasks[i]->Push([&completed] { ++completed; });
            DestroyTasks(th);          // hangs here if the worker slept through the notify
        }
        finished = true;
    });
    auto t0 = clock_::now();
    while (!finished && msSince(t0) < 15000)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!finished) {
        std::printf("      J3: STUCK after %ld ms at round %d/%d — Destroy() never returned\n",
                    msSince(t0), completed.load(), kRounds);
        ok(false, "J3: destroying the task pool always completes (lost wakeup = hung encoder thread)");
        // The stuck thread can never be joined; let the process exit reap it rather than hang the
        // harness until `timeout 90` kills it and the log comes back empty.
        std::fflush(stdout);
        std::_Exit(1);
    }
    hammer.join();
    std::printf("      J3: %d create/destroy rounds, %d tasks executed, %ld ms\n",
                kRounds, completed.load(), msSince(t0));
    // How many of the pushed tasks got to run before the teardown is genuinely racy — Destroy is
    // allowed to stop a worker that has not drained its queue — so it is printed, not asserted.
    // What must never vary is the left-hand side of that line: reaching it at all means every
    // Destroy() returned.
    ok(true, "J3: 3000 create/destroy rounds — every Destroy() returned");
}

} // namespace

int main() {
    // Line-buffer the log: the failure mode here is a HANG, and a killed process never flushes a
    // block buffer — the run log comes back empty and the hang is undiagnosable.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::printf("thread-task-join: Join() waits for the work, Destroy() waits for the thread\n");

    watchdog("J1", 30, doneJ1);
    test_join_survives_a_spurious_wake();
    doneJ1 = true;

    watchdog("J2", 40, doneJ2);
    test_join_waits_for_every_task();
    doneJ2 = true;

    watchdog("J3", 40, doneJ3);
    test_destroy_never_hangs();
    doneJ3 = true;

    std::printf("\nthread-task-join: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
