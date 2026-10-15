/*
 * vmx-saveto-contract.cpp — VMX_SaveTo() answers failure with 0. Not with an error code that
 * happens to be a positive number.
 *
 * Why this exists (P0 #10, ported from the desktop vendored copy): vmxcodec.cpp used to begin
 *
 *     if (!instance) return VMX_ERR_INVALID_INSTANCE;      // 5
 *     if (!dst) return VMX_ERR_INVALID_PARAMETERS;         // 6
 *     if (!maxLen) return VMX_ERR_INVALID_PARAMETERS;      // 6
 *
 * while every caller of VMX_SaveTo treats the return value as a byte count. Sender::encodeFrame()
 * returned it straight to Sender::send(), which tested only `encodedLen <= 0` and then built a
 * FrameHeader + VideoExtHeader announcing a real video frame with that many payload bytes, and
 * queued it to every subscriber on the link. A 5-byte "64x64 BGRA frame" is not a frame; it is an
 * error code wearing a header, and the receiver — not the sender — is where anyone finds out.
 *
 * The second half of the same hunk is CHECKBUFF_SAVE. The old macro was
 *
 *     if ((b + checkNumBytes) > maxB) return 0;
 *
 * which forms the out-of-range pointer before comparing it (undefined), and which a negative
 * length wins outright: b + (-8) sits below maxB, the check passes, and the memcpy underneath it is
 * handed (size_t)(-8) as its length. The replacement compares the bytes that are left, and refuses
 * a negative count. Part C proves that difference is observable rather than cosmetic: it pokes a
 * slice whose StreamPos sits 8 bytes below Stream, and the fixed macro returns 0 while the old one
 * dies in ASan. (The macro's `_need < 4` test is also what makes the desktop copy's uint32_t
 * `len` safe here — that form wraps `len + 4` to 0 in 32-bit arithmetic, and 0 is refused as a
 * write too. `len` is kept signed in this tree so the negative is visible at the loop as well.)
 *
 * Build + run:  bash qa/build-host.sh   (which also runs the mutation controls)
 */
#include "OMTSender.h"
#include "OMTProtocol.h"
#include "vmxcodec.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace omt;

static int failures = 0;
static void ok(bool cond, const std::string& what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++failures;
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);

    std::vector<uint8_t> buf(1 << 20, 0);
    const int cap = (int)buf.size();

    // ---------- A: the failure contract, before any encoder exists ----------
    ok(VMX_SaveTo(nullptr, buf.data(), cap) == 0,
       "A1: a null instance returns 0 — not VMX_ERR_INVALID_INSTANCE (5), which a caller testing "
       "`len <= 0` would publish as a five-byte video frame");
    ok(VMX_SaveTo(nullptr, buf.data(), cap) != VMX_ERR_INVALID_INSTANCE,
       "A2: stated as the trap itself: the return value must not be the error enum");
    ok(VMX_SaveTo(nullptr, nullptr, cap) == 0, "A3: a null destination returns 0 too");

    VMX_SIZE dims = {64, 64};
    VMX_INSTANCE* inst = VMX_Create(dims, VMX_PROFILE_OMT_SQ, VMX_COLORSPACE_BT709);
    if (!inst) { std::printf("  [FAIL] VMX_Create(64x64) failed on the host build\n"); return 1; }

    std::vector<uint8_t> px((size_t)64 * 64 * 4);
    uint32_t rng = 0x0DD12345u;
    for (auto& b : px) { rng = rng * 1103515245u + 12345u; b = (uint8_t)(rng >> 16); }
    ok(VMX_EncodeBGRA(inst, px.data(), 64 * 4, 0) == VMX_ERR_OK, "a 64x64 BGRA frame encodes");

    ok(VMX_SaveTo(inst, nullptr, cap) == 0,
       "A4: a null destination returns 0, not VMX_ERR_INVALID_PARAMETERS (6)");
    ok(VMX_SaveTo(inst, nullptr, cap) != VMX_ERR_INVALID_PARAMETERS,
       "A5: and again, not the error enum");
    ok(VMX_SaveTo(inst, buf.data(), 0) == 0, "A6: maxLen == 0 returns 0, not 6");
    ok(VMX_SaveTo(inst, buf.data(), -8) == 0,
       "A7: a NEGATIVE maxLen returns 0 — the old `if (!maxLen)` let it through, and maxB = dst + "
       "maxLen then fell BELOW b, so the bounds macro was comparing pointers from opposite ends");

    // ---------- B: the happy path still works (nothing here may be a gate that rejects everything)
    {
        int n = VMX_SaveTo(inst, buf.data(), cap);
        ok(n > 0, "B1: a real frame still saves (" + std::to_string(n) + " bytes)");
        ok(n <= cap, "B2: and the length is inside the buffer we handed it");
        // The first byte is the codec format tag; if the save were fabricated it would not be.
        ok(buf[0] == VMX_CODEC_FORMAT_PROGRESSIVE || buf[0] == VMX_CODEC_FORMAT_EXTENDED ||
           buf[0] == VMX_CODEC_FORMAT_INTERLACED,
           "B3: and the header it wrote is a codec header (tag " + std::to_string(buf[0]) + ")");
        // A capacity smaller than the 3/5-byte header cannot be written at all.
        ok(VMX_SaveTo(inst, buf.data(), 2) == 0,
           "B4: a capacity smaller than the frame header returns 0 rather than writing anyway");
    }

    // ---------- C: the bounds macro against a hostile slice ----------
    // A slice whose StreamPos sits below Stream is memory corruption or a codec bug, not a normal
    // input — which is exactly why the macro has to be written so that it cannot be won. The old
    // `(b + n) > maxB` form answers "is a pointer 8 bytes BEFORE the buffer past its end?" — no —
    // and then hands memcpy (size_t)(-8). The remaining-bytes form refuses the negative outright.
    {
        ok(VMX_EncodeBGRA(inst, px.data(), 64 * 4, 0) == VMX_ERR_OK, "re-encode before the probe");
        VMX_SLICE_DATA& dc = inst->Slices[0]->DC;
        BYTE* savedPos = dc.StreamPos;
        dc.StreamPos = dc.Stream - 8;                     // the corruption, injected
        int n = VMX_SaveTo(inst, buf.data(), cap);
        dc.StreamPos = savedPos;                          // leave the instance destroyable
        ok(n == 0, "C1: a slice with a negative encoded length is refused with 0 — reaching this "
                   "line at all is the assertion; with the old pointer-arithmetic macro this run "
                   "dies in memcpy(dst, Stream, (size_t)-8) instead");

        // C2: the same probe on the AC plane, so the second loop is covered too.
        ok(VMX_EncodeBGRA(inst, px.data(), 64 * 4, 0) == VMX_ERR_OK, "re-encode again");
        VMX_SLICE_DATA& ac = inst->Slices[0]->AC;
        BYTE* savedAc = ac.StreamPos;
        ac.StreamPos = ac.Stream - 4;
        n = VMX_SaveTo(inst, buf.data(), cap);
        ac.StreamPos = savedAc;
        ok(n == 0, "C2: and the same for the AC loop, which is the second copy of the macro");
    }

    VMX_Destroy(inst);

    // ---------- D: the sender-side gate ----------
    // VMX_SaveTo() bounds itself, so a length outside (0, capacity] cannot reach the publisher from
    // the inside; this is the belt. Asserted directly because the only way to produce a lying codec
    // from here is to link a fake one, and a fake that returns 4096 proves nothing about 2^31.
    {
        std::vector<uint8_t> big(4096);
        ok(Sender::encodedLenAcceptableForTest(4096, big.size()),
           "D1: a payload that exactly fills the buffer is publishable");
        ok(!Sender::encodedLenAcceptableForTest(4097, big.size()),
           "D2: one byte more than the buffer is not — that is the length that used to be trusted "
           "and stamped into FrameHeader::dataLength for every subscriber to read");
        ok(!Sender::encodedLenAcceptableForTest(0, big.size()),
           "D3: 0 (the failure answer) is not");
        ok(!Sender::encodedLenAcceptableForTest(-1, big.size()),
           "D4: and neither is a negative, which the pre-fix VMX_ERR_* returns would have become "
           "on a 32-bit size_t");
        ok(!Sender::encodedLenAcceptableForTest(VMX_ERR_INVALID_INSTANCE, 0),
           "D5: a five-byte claim against a zero-capacity buffer is refused — the exact shape of "
           "the frame this defect put on the wire");
        ok(!Sender::encodedLenAcceptableForTest((int)(big.size() + 1), big.size()),
           "D6: the comparison is done in size_t, so a length that would wrap the comparison on a "
           "narrower type is still refused");
    }

    std::printf("\nvmx-saveto-contract: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
