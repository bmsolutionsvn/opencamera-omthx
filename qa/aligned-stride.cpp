/*
 * aligned-stride.cpp — proves the libvmx aligned-stride helpers act on the buffers they actually
 * own, and that the row tail the SIMD block loader reads past the last pixel is initialised.
 * (P0 #12, ported from the desktop vendored copy at
 *  nodos-yded-linux/plugins/yd.net.ompout/vendor/libvmx/src/vmxcodec_common.h.)
 *
 * Why this exists: VMX_CreateAlignedStrideBuffer() mallocs iff (width % alignment) — the caller's
 * stride plays no part in that decision. VMX_CopyToAlignedStrideBuffer(), VMX_FreeAlignedStrideBuffer()
 * and VMX_CopyFromAlignedStrideBufferAndFree() used to decide by (alignedStride != srcStride)
 * instead, and the two predicates disagree whenever a HAL already pads its row stride up to the
 * alignment while the width is not a multiple of it. 1080 and 2592 are real camera widths that do
 * exactly that (1080 % 64 = 24, 2592 % 64 = 32). In that state the malloc happened, yet every
 * predicate says "not ours":
 *
 *   - the copy is skipped, so the NEON/SSE encoder runs over an UNINITIALIZED malloc'd buffer —
 *     the subscriber sees a black or garbage frame and nothing anywhere reports an error;
 *   - the buffer is never freed, so every frame leaks alignedStride * height bytes;
 *   - on the decode side the copy-back is skipped too, so the decoded image is thrown away and the
 *     caller's buffer keeps its previous contents.
 *
 * Separately, for every padded width the bytes [rowBytes, alignedStride) of each row were left
 * uninitialised after the copy. The block loaders walk the row in 16/32/64-byte vector loads up to
 * the aligned stride (see VMX_UYVYToPlanar: `for (x = 0; x < width*2; x += 64)` reading pSrc[x..x+63]),
 * so they consume indeterminate bytes and the last block of every row encodes non-deterministically.
 *
 * The predicates are now pointer identity against the original buffer — the same question Create
 * asked when it decided to malloc — and the copy memsets the tail. The inline is shared by the
 * x86 (SSE2/AVX2) and the ARM NEON path, and the ARM NEON path is what the APK builds.
 *
 * Build + run on the host (no device, no JVM):  bash qa/build-host.sh
 * Expected: ALL PASS on the fixed code. With the fix reverted on a copy of the tree the same
 * binary must go red — A1/A4 by the ASan leak report, A2 by the poison assertion, B by the
 * bitstream comparison. That A/B is what proves the test has teeth.
 */
#include "vmxcodec.h"
#include "vmxcodec_common.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

int failures = 0;

void ok(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++failures;
}

// Create rounds the width up to a multiple of the alignment and reports width*bytesPerPixel.
// 1080 -> 1088 (tail 16 B at 2 bytes/pixel), 2592 -> 2624 (tail 64 B): both are real camera widths.
int alignUp(int v, int a) { while (v % a) ++v; return v; }

// A pattern the encoder cannot mistake for uninitialised memory: neither 0x00 (fresh pages) nor
// 0x5C (the poison written into the recycled chunk below), and different in every byte position.
unsigned char pixel(int x, int y) {
    return static_cast<unsigned char>((x * 7 + y * 13 + (x >> 3) * 5) & 0xFF);
}

// Hand malloc a chunk of exactly the size the codec is about to request, fill it with a loud
// pattern, and free it — so the codec's malloc is served from memory whose contents are known to
// be wrong. Without the copy this is what the encoder ends up encoding.
void poisonAllocatorFor(size_t bytes) {
    unsigned char* junk = static_cast<unsigned char*>(std::malloc(bytes));
    if (!junk) return;
    std::memset(junk, 0x5C, bytes);
    std::free(junk);
}

// ------------------------------------------------------------------- B: end-to-end determinism ---
// The same image, encoded twice through the production entry point VMX_EncodeUYVY(): once with the
// tight stride the codec's own copy predicate is happy with, once with the stride a padded HAL
// reports. Identical pixels must produce an identical bitstream. Pre-fix the padded run encodes
// uninitialised memory, so the two differ — and the scratch buffer leaks.
std::vector<unsigned char> encodeUYVY(int w, int h, int stride, const unsigned char* image,
                                      int* outLen) {
    VMX_INSTANCE* inst = VMX_Create(VMX_SIZE{w, h}, VMX_PROFILE_OMT_SQ, VMX_COLORSPACE_BT709);
    if (!inst) { *outLen = -1; return {}; }
    std::vector<unsigned char> buf(4u * 1024u * 1024u);
    VMX_ERR err = VMX_EncodeUYVY(inst, const_cast<unsigned char*>(image), stride, 0);
    int n = (err == VMX_ERR_OK) ? VMX_SaveTo(inst, buf.data(), (int)buf.size()) : 0;
    VMX_Destroy(inst);
    *outLen = n;
    buf.resize(n > 0 ? (size_t)n : 0);
    return buf;
}

// ---------------------------------------------------------------- A1: the HAL-padded source ---
// width 1080, alignment 64, 2 bytes/pixel (the UYVY luma+chroma row): Create mallocs because
// 1080 % 64 != 0, and the HAL already padded the row to that very stride. Pre-fix the predicate
// (alignedStride != srcStride) is FALSE -> no copy (encoder eats uninitialised memory) and no
// free (ASan reports the leak at exit).
void test_padded_source_is_still_copied() {
    const int w = 1080, h = 256, bpp = 2, alignment = 64;
    const int rowBytes = w * bpp;                        // 2160
    const int alignedStride = alignUp(w, alignment) * bpp;   // 2176 — what Create reports

    std::vector<unsigned char> src((size_t)alignedStride * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < rowBytes; ++x)
            src[(size_t)y * alignedStride + x] = pixel(x, y);

    poisonAllocatorFor((size_t)alignedStride * h);

    unsigned char* aligned = nullptr;
    int stride = 0;
    VMX_SIZE size{w, h};
    VMX_CreateAlignedStrideBuffer(src.data(), alignedStride, size, &aligned, &stride, alignment, bpp);

    ok(stride == alignedStride, "A1a: Create reports the stride the row was aligned to");
    ok(aligned != src.data(), "A1b: Create malloced a buffer of its own (width % alignment != 0)");

    VMX_CopyToAlignedStrideBuffer(aligned, stride, src.data(), alignedStride, size, bpp);

    int badRow = -1, badCol = -1;
    for (int y = 0; y < h && badRow < 0; ++y)
        for (int x = 0; x < rowBytes; ++x)
            if (aligned[(size_t)y * stride + x] != src[(size_t)y * alignedStride + x]) {
                badRow = y; badCol = x; break;
            }
    std::printf("      A1: srcStride == alignedStride == %d, rowBytes %d; first byte the copy "
                "missed at row %d col %d (buffer holds 0x%02x, image wants 0x%02x)\n",
                alignedStride, rowBytes, badRow, badCol,
                badRow >= 0 ? aligned[(size_t)badRow * stride + badCol] : 0,
                badRow >= 0 ? src[(size_t)badRow * alignedStride + badCol] : 0);
    ok(badRow < 0, "A1c: the row WAS copied into the owned buffer (pre-fix: predicate said 'not ours')");

    // The test, not the helper, releases this one: VMX_FreeAlignedStrideBuffer's third parameter
    // changed type with the fix (int srcStride -> const BYTE* original), and the scenarios here
    // have to compile unchanged against BOTH headers so the A/B compares behaviour, not the build.
    // "the helper must free what it allocated" is gated by A4 and B1, which go through the codec's
    // own entry points and leak visibly (LeakSanitizer) when the predicate is wrong.
    std::free(aligned);
}

// ------------------------------------------------------------------------ A2: the SIMD tail ---
// The destination buffer here is supplied by the test and deliberately poisoned, so "the tail was
// left uninitialised" is a deterministic content check rather than a hope that malloc returns
// garbage. This is the case that makes the last block of every row encode non-deterministically.
void test_row_tail_is_zeroed() {
    const int w = 2592, h = 64, bpp = 2, alignment = 64;   // the other real camera width
    const int rowBytes = w * bpp;                        // 5184
    const int alignedStride = alignUp(w, alignment) * bpp; // 5248 — 64 bytes of tail per row

    std::vector<unsigned char> src((size_t)rowBytes * h, 0);
    for (size_t i = 0; i < src.size(); ++i) src[i] = pixel((int)(i % rowBytes), (int)(i / rowBytes));

    unsigned char* aligned = static_cast<unsigned char*>(std::malloc((size_t)alignedStride * h));
    if (!aligned) { ok(false, "A2: out of memory"); return; }
    std::memset(aligned, 0x5C, (size_t)alignedStride * h);   // the state malloc leaves it in

    VMX_SIZE size{w, h};
    VMX_CopyToAlignedStrideBuffer(aligned, alignedStride, src.data(), rowBytes, size, bpp);

    bool rowsMatch = true, tailZero = true;
    int firstBadRow = -1, firstBadTailRow = -1, firstBadTail = -1;
    for (int y = 0; y < h; ++y) {
        if (std::memcmp(aligned + (size_t)y * alignedStride, src.data() + (size_t)y * rowBytes,
                        (size_t)rowBytes) != 0) {
            if (firstBadRow < 0) firstBadRow = y;
            rowsMatch = false;
        }
        for (int x = rowBytes; x < alignedStride; ++x) {
            if (aligned[(size_t)y * alignedStride + x] != 0) {
                if (firstBadTail < 0) { firstBadTail = x; firstBadTailRow = y; }
                tailZero = false;
                break;
            }
        }
    }
    std::printf("      A2: rowBytes=%d alignedStride=%d tail=%d bytes/row; first bad row=%d, "
                "first non-zero tail byte at column %d (value 0x%02x)\n",
                rowBytes, alignedStride, alignedStride - rowBytes, firstBadRow, firstBadTail,
                (firstBadTailRow >= 0 && firstBadTail >= 0)
                    ? aligned[(size_t)firstBadTailRow * alignedStride + firstBadTail] : 0);
    ok(rowsMatch, "A2a: every row's valid pixels were copied");
    ok(tailZero, "A2b: the tail the vector loads read past the pixels is zeroed, not indeterminate");

    std::free(aligned);
}

// ------------------------------------------------------- A3: a buffer we do NOT own stays put ---
// Width already a multiple of the alignment: Create borrows the caller's own image, it never
// mallocs. The helpers must not copy onto it and, above all, must not free() it — free()ing a
// pointer nobody malloc'd is an immediate ASan death. This is the case that proves the new
// predicate is *ownership* and not merely "always release the scratch buffer".
void test_aligned_width_does_not_free_the_callers_image() {
    const int w = 1088, h = 256, bpp = 2;      // 1088 % 64 == 0: Create borrows, never mallocs
    const int stride = w * bpp;

    std::vector<unsigned char> src((size_t)stride * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < stride; ++x)
            src[(size_t)y * stride + x] = pixel(x, y);
    const std::vector<unsigned char> before = src;

    int n = 0;
    poisonAllocatorFor((size_t)stride * h);
    std::vector<unsigned char> bits = encodeUYVY(w, h, stride, src.data(), &n);

    ok(n > 0, "A3a: an already-aligned width still encodes");
    ok(std::memcmp(src.data(), before.data(), src.size()) == 0,
       "A3b: the borrowed caller buffer survived the encode — freeing what we do not own is an "
       "ASan death here, which is why ownership must be pointer identity");
}

// --------------------------------------------- A4: the decode-side copy-back must actually run ---
// The mirror of A1: VMX_CopyFromAlignedStrideBufferAndFree() skipped both the copy and the free
// for a stride-padded destination, so the decoded frame was silently discarded and the scratch
// buffer leaked.
void test_padded_destination_gets_the_pixels() {
    const int w = 1080, h = 128, bpp = 2, alignment = 64;
    const int rowBytes = w * bpp;
    const int alignedStride = alignUp(w, alignment) * bpp;

    unsigned char* aligned = nullptr;
    int stride = 0;
    std::vector<unsigned char> dst((size_t)alignedStride * h, 0x11);
    VMX_SIZE size{w, h};
    poisonAllocatorFor((size_t)alignedStride * h);
    VMX_CreateAlignedStrideBuffer(dst.data(), alignedStride, size, &aligned, &stride, alignment, bpp);

    // What the codec wrote into its scratch rows:
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < rowBytes; ++x)
            aligned[(size_t)y * stride + x] = pixel(x, y + 1);

    VMX_CopyFromAlignedStrideBufferAndFree(aligned, stride, dst.data(), alignedStride, size, bpp);

    bool pixelsLanded = true;
    for (int y = 0; y < h && pixelsLanded; ++y)
        for (int x = 0; x < rowBytes; ++x)
            if (dst[(size_t)y * alignedStride + x] != pixel(x, y + 1)) { pixelsLanded = false; break; }
    ok(pixelsLanded, "A4a: the decoded pixels reached the caller's stride-padded buffer");
    // A4's second half is the free: pre-fix the buffer is orphaned and LSan fails the run.
}

void test_padded_and_tight_encode_agree() {
    const int w = 1080, h = 1920, bpp = 2;      // a phone in portrait: real camera geometry
    const int rowBytes = w * bpp;                        // 2160
    const int alignedStride = alignUp(w, 64) * bpp;      // 2176 — the stride a HAL pads to

    std::vector<unsigned char> tight((size_t)rowBytes * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < rowBytes; ++x)
            tight[(size_t)y * rowBytes + x] = pixel(x, y);

    std::vector<unsigned char> padded((size_t)alignedStride * h, 0);   // tail explicitly zero
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < rowBytes; ++x)
            padded[(size_t)y * alignedStride + x] = tight[(size_t)y * rowBytes + x];

    poisonAllocatorFor((size_t)alignedStride * h);
    int nTight = 0, nPadded = 0;
    std::vector<unsigned char> bitsTight = encodeUYVY(w, h, rowBytes, tight.data(), &nTight);
    poisonAllocatorFor((size_t)alignedStride * h);
    std::vector<unsigned char> bitsPadded = encodeUYVY(w, h, alignedStride, padded.data(), &nPadded);

    std::printf("      B: 1080x1920 UYVY — tight stride %d -> %d bytes, HAL stride %d -> %d bytes\n",
                rowBytes, nTight, alignedStride, nPadded);
    ok(nTight > 0 && nPadded > 0, "B1: both encodes produced a payload");
    bool same = (bitsTight.size() == bitsPadded.size()) &&
                (bitsTight.empty() ||
                 std::memcmp(bitsTight.data(), bitsPadded.data(), bitsTight.size()) == 0);
    if (!same && !bitsTight.empty() && !bitsPadded.empty()) {
        size_t n = std::min(bitsTight.size(), bitsPadded.size());
        size_t d = 0;
        while (d < n && bitsTight[d] == bitsPadded[d]) ++d;
        std::printf("      B: bitstreams diverge at byte %zu of %zu/%zu (0x%02x vs 0x%02x)\n",
                    d, bitsTight.size(), bitsPadded.size(),
                    d < bitsTight.size() ? bitsTight[d] : 0, d < bitsPadded.size() ? bitsPadded[d] : 0);
    }
    ok(same, "B2: the same pixels encode identically whether or not the HAL padded the stride");
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::printf("aligned-stride: ownership predicate and SIMD row tail\n");

    test_padded_source_is_still_copied();
    test_row_tail_is_zeroed();
    test_aligned_width_does_not_free_the_callers_image();
    test_padded_destination_gets_the_pixels();
    test_padded_and_tight_encode_agree();

    std::printf("\naligned-stride: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASS", failures);
    // Anything still reachable here is a leak the codec will repeat on every frame: LeakSanitizer
    // turns it into a non-zero exit after main returns.
    return failures ? 1 : 0;
}
