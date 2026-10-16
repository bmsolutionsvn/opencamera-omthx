/*
 * vmx-thread-cap.cpp — the codec may only fan out to CPUs this process can actually run on.
 *
 * THE FAILURE THIS MEASURES. VMX_EncodePlanes (libvmx/src/vmxcodec.cpp:~1700) pushes one slice range
 * per thread and then Joins EVERY one of them: the frame is not finished until the slowest slice is.
 * The thread count it fans out to comes from VMX_BITRATE_TABLE (2 at <=1440p, 4 at 2160p, 8 at
 * 4320p) and is then overridden to 8 or 16 for 2160p/4320p whenever std::thread::hardware_concurrency()
 * reports that many CPUs. hardware_concurrency() answers "how many cpus are ONLINE" — it does not
 * know that a cpuset-restricted process has four, or that a phone's scheduler has parked six of
 * eight cores. Ask for sixteen slice threads on a box that can run four and every frame pays two
 * rounds of scheduling for the second twelve while Join holds the whole frame open. On the phone
 * that is the shape of "delivered fps is half of what the camera was told to produce"; the phone is
 * not here today, so what this file proves is the host-provable half of that claim, and the commit
 * says plainly which half is unproven.
 *
 * WHAT IS UNDER TEST IS THE REAL CODEC: libvmx/src/vmxcodec.cpp, compiled in, no seam. The only
 * thing the scenario decides is how many CPUs the process is allowed to run on, which it does with
 * the kernel's own affinity mask — the same knob taskset -c turns, and the same one a container or
 * an Android cpuset governor turns.
 *
 * THE CASES:
 *   C1  every create is capped: 1 <= VMX_GetThreads() <= the CPUs this process may run on. This is
 *       the gate. Under `taskset -c 0-3` the pre-cap codec answers 8 for 2160p and 16 for 4320p,
 *       which is what et/tc-prefix is scored red on; on an unrestricted 32-way box the same binary
 *       is green there, which is why the section runs it BOTH ways rather than picking the easy one.
 *   C2  the clamp is real: never zero. VMX_EncodePlanes divides totalSlices by this number, so a
 *       zero is a SIGFPE on the first frame, and hardware_concurrency() is allowed to return 0.
 *   C3  the cap changed the fan-out and nothing else: the same input encoded at 1, 2, 4, 8 and 16
 *       threads must produce the SAME bytes. SliceCount (vmxcodec.cpp:409) and the slice ranges are
 *       the wire; the number of tasks they are pushed to is not. If this case ever goes red, the
 *       cap is no longer a scheduling decision and must not ship as one.
 *   C4  the pool really works at the capped size — the encode completes and SaveTo returns payload.
 *       A cap that quietly starved the codec into an empty bitstream would pass C1.
 *   C5  the wall clock, printed and not asserted: ms/frame at each thread count. Oversubscription is
 *       what the cap is for, so these are the numbers the commit message quotes; asserting on them
 *       would put a scheduler's mood into the gate. (qa/repeat-run.sh is what keeps the ASSERTIONS
 *       honest; the timings are evidence, not a verdict.)
 *
 * Build + run: bash qa/build-host.sh   (the tc-* section, which also runs the pre-cap control under
 * a 4-CPU affinity mask and the mutants that remove the cap or force an absurd thread count).
 */
#include "vmxcodec.h"

#include <sched.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;
void ok(bool cond, const std::string& what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++failures;
}

/* The number the cap is supposed to honour. Computed here, from the kernel, independently of the
 * codec's own helper: if the codec and the test agreed by sharing a bug, this file would prove
 * nothing. */
int runnableCpus() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        int n = CPU_COUNT(&set);
        if (n >= 1) return n;
    }
    unsigned n = std::thread::hardware_concurrency();
    return n > 0 ? (int)n : 1;
}

/* What the pre-cap codec would have asked for at this height: the override in VMX_Create, which
 * hardware_concurrency() (not the affinity mask) decides. Recomputed from the same public API the
 * product uses, so the assertion below names a concrete number rather than "not what I expected". */
int hardwareCpus() {
    unsigned n = std::thread::hardware_concurrency();
    return n > 0 ? (int)n : 1;
}

struct Encoded {
    std::vector<uint8_t> bytes;
    bool ok() const { return !bytes.empty(); }
};

/** One 1080p NV12 frame, encoded with the real codec, exactly as Sender::encodeFrame() drives it:
 *  luma at stride, chroma at stride*height, SaveTo into a buffer. */
Encoded encodeOneFrame(VMX_INSTANCE* inst, int w, int h, int stride,
                       const std::vector<uint8_t>& src, double* msOut) {
    Encoded out;
    const int uvOff = stride * h;
    auto t0 = std::chrono::steady_clock::now();
    VMX_ERR err = VMX_EncodeNV12(inst, const_cast<BYTE*>(src.data()), stride,
                                const_cast<BYTE*>(src.data() + uvOff), stride, 0);
    if (err != VMX_ERR_OK) {
        std::printf("      VMX_EncodeNV12 returned %d\n", (int)err);
        return out;
    }
    /* VMX_SaveTo answers a buffer it cannot fit with 0 — the same 0 as any other failure
     * (vmxcodec.cpp's CHECKBUFF_SAVE) — so a fixed 4 MB reads as "the encoder produced nothing" at
     * 2160p. One byte per pixel is above what any setting in VMX_BITRATE_TABLE can ask for. */
    std::vector<uint8_t> buf((size_t)w * (size_t)h);
    int n = VMX_SaveTo(inst, buf.data(), (int)buf.size());
    if (msOut) {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        *msOut = (double)us / 1000.0;
    }
    if (n <= 0) return out;
    buf.resize((size_t)n);
    out.bytes = std::move(buf);
    return out;
}

std::vector<uint8_t> makeFrame(int stride, int h) {
    std::vector<uint8_t> v((size_t)stride * h * 3 / 2);
    for (size_t i = 0; i < v.size(); ++i) v[i] = (uint8_t)((i * 31 + (i >> 6) * 7) & 0xFF);
    return v;
}

const int W = 1920, H = 1080, STRIDE = 1920;

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const int runnable = runnableCpus();
    const int hw = hardwareCpus();
    std::printf("vmx-thread-cap: real libvmx, %d online cpus, %d this process may run on\n", hw, runnable);

    /* ── C1/C2: the cap, at every resolution the table treats specially ─────────────────────── */
    struct Size { int h; const char* name; int preCapWants; };
    /* preCapWants is what VMX_Create chose before the cap existed: the table value, or the
     * hardware_concurrency() override that fires for 2160p/4320p. Only the two tall rows can
     * exceed a small affinity mask, which is why the prefix control is run constrained. */
    const Size sizes[] = {
        { 720, "1280x720",  2 },
        {1080, "1920x1080", 2 },
        {2160, "3840x2160", 8 },
        {4320, "7680x4320", 16 },
    };
    for (const Size& s : sizes) {
        VMX_SIZE dim{}; dim.width = (s.h == 720) ? 1280 : (s.h * 16 / 9); dim.height = s.h;
        VMX_INSTANCE* inst = VMX_Create(dim, VMX_PROFILE_OMT_HQ, VMX_COLORSPACE_BT709);
        if (!inst) { ok(false, std::string("C1 VMX_Create refused ") + s.name); continue; }
        const int got = VMX_GetThreads(inst);
        ok(got >= 1, std::string("C2 ") + s.name + ": the worker count is at least 1 — VMX_EncodePlanes "
                                 "divides the slice count by it (" + std::to_string(got) + ")");
        ok(got <= runnable, std::string("C1 ") + s.name + ": the codec asked for no more slice threads "
                            "than this process can run on (" + std::to_string(got) + " <= "
                            + std::to_string(runnable) + "; the uncapped codec would have asked for "
                            + std::to_string(s.preCapWants) + ")");
        VMX_Destroy(inst);
    }

    /* ── C3/C4/C5: fan-out is a scheduling decision, not a wire decision ─────────────────────── */
    VMX_SIZE dim{}; dim.width = W; dim.height = H;
    /*
     * Each thread count gets its OWN instance and its OWN frame sequence, and the comparison is on
     * the FIRST frame. That is not fussiness: the codec is stateful across frames (the rate
     * controller steers toward TargetBytesPerFrame and the conditional-replenishment path reads the
     * previous frame), so the 2nd..Nth payload of one instance describes what the encoder had
     * already sent. An earlier draft of this case encoded successive frames on one instance and
     * "proved" the fan-out changed the bitstream — what it proved is that the controller adapts, and
     * it printed a payload that shrank from 1.72 MB to 1.18 MB as the frames went by. Same frame,
     * same instance, same starting state, different number of tasks: that is the only comparison
     * that says anything about the cap.
     */
    const std::vector<uint8_t> src = makeFrame(STRIDE, H);
    const int FRAMES = 12;
    bool same = true;
    std::vector<uint8_t> reference;
    for (int t : {1, 2, 4, 8, 16}) {
        VMX_INSTANCE* inst = VMX_Create(dim, VMX_PROFILE_OMT_HQ, VMX_COLORSPACE_BT709);
        if (!inst) { ok(false, "C4 VMX_Create refused 1920x1080 at " + std::to_string(t) + " threads"); same = false; break; }
        VMX_SetThreads(inst, t);
        if (VMX_GetThreads(inst) != t) {
            ok(false, "C3 VMX_SetThreads(" + std::to_string(t) + ") did not take: "
                      + std::to_string(VMX_GetThreads(inst)));
            same = false; VMX_Destroy(inst); break;
        }
        double totalMs = 0.0;
        std::vector<uint8_t> first;
        bool all_ok = true;
        for (int k = 0; k < FRAMES; ++k) {
            double ms = 0.0;
            Encoded e = encodeOneFrame(inst, W, H, STRIDE, src, &ms);
            if (!e.ok()) all_ok = false;
            if (k == 0) first = std::move(e.bytes);
            totalMs += ms;
        }
        ok(all_ok, "C4 every one of " + std::to_string(FRAMES) + " encodes completed and SaveTo "
                   "returned payload at " + std::to_string(t) + " worker threads ("
                   + std::to_string(first.size()) + " bytes for the first)");
        if (reference.empty()) reference = first;
        else if (reference != first) {
            same = false;
            ok(false, "C3 the FIRST frame's payload at " + std::to_string(t) + " threads differs from "
                      "the one at 1 thread — the task fan-out leaked into the bitstream, which is not "
                      "a scheduling decision any more");
        }
        std::printf("      timing threads=%-2d  %.3f ms/frame over %d frames  (first payload %zu bytes)\n",
                    t, totalMs / FRAMES, FRAMES, first.size());
        VMX_Destroy(inst);
    }
    ok(same, "C3 the same 1080p frame encodes to the SAME bytes at 1, 2, 4, 8 and 16 worker threads: "
             "the cap moves work between threads, it does not move anything on the wire");

    /* ── C6: the cap is what the affinity mask says, printed so the run log can be checked ───── */
    VMX_INSTANCE* tall = VMX_Create(dim, VMX_PROFILE_OMT_HQ, VMX_COLORSPACE_BT709);
    if (tall) {
        std::printf("      1080p OMT_HQ: threads=%d (table value 2, capped at %d runnable, %d online)\n",
                    VMX_GetThreads(tall), runnable, hw);
        VMX_Destroy(tall);
    }
    VMX_SIZE uhd{}; uhd.width = 3840; uhd.height = 2160;
    VMX_INSTANCE* uhdInst = VMX_Create(uhd, VMX_PROFILE_OMT_HQ, VMX_COLORSPACE_BT709);
    if (uhdInst) {
        std::printf("      2160p OMT_HQ: threads=%d (uncapped codec asks for 8 on any box with 8 online, "
                    "capped at %d runnable)\n", VMX_GetThreads(uhdInst), runnable);
        VMX_Destroy(uhdInst);
    }

    /* ── C7: the cap's OWN wall clock, at the resolution where the cap can bite ─────────────────
     *
     * C3..C5 call VMX_SetThreads explicitly, which is what makes the byte-identity proof possible
     * and what makes it useless for THIS question: the cap decides the count VMX_Create comes up
     * with, and SetThreads overwrites that decision. So this measures the instance exactly as
     * Sender::createEncoder() gets it — no SetThreads anywhere — at 2160p, the smallest height whose
     * table/override value can exceed a restricted process. Run the same binary twice, once on the
     * whole machine and once under `taskset -c 0-3`, and the line below is the cap's actual effect
     * on the shipped code path: the pre-cap codec prints "asked for 8, 8 used" on the 4-cpu budget,
     * the capped codec prints "asked for 8, 4 used".
     */
    {
        const int TW = 3840, TH = 2160, TSTRIDE = 3840;
        const std::vector<uint8_t> big = makeFrame(TSTRIDE, TH);
        VMX_SIZE bigDim{}; bigDim.width = TW; bigDim.height = TH;
        VMX_INSTANCE* inst = VMX_Create(bigDim, VMX_PROFILE_OMT_HQ, VMX_COLORSPACE_BT709);
        if (!inst) {
            ok(false, "C7 VMX_Create refused 3840x2160 — the cap's own timing cannot be measured");
        } else {
            const int chosen = VMX_GetThreads(inst);
            double totalMs = 0.0; bool all = true;
            const int K = 6;
            for (int k = 0; k < K; ++k) {
                double ms = 0.0;
                Encoded e = encodeOneFrame(inst, TW, TH, TSTRIDE, big, &ms);
                if (!e.ok()) all = false;
                totalMs += ms;
            }
            ok(all, "C7 the as-created encoder produced a payload for all " + std::to_string(K)
                    + " 2160p frames at " + std::to_string(chosen) + " threads");
            std::printf("      as-created 2160p: threads=%d (the codec asked for 8)  %.3f ms/frame over %d frames"
                        "   [runnable=%d online=%d]\n",
                        chosen, totalMs / K, K, runnable, hw);
            VMX_Destroy(inst);
        }
    }

    std::printf("\nvmx-thread-cap: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
