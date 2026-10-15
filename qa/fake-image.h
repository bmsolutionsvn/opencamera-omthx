/*
 * fake-image.h — TEST-ONLY. A Camera2 YUV_420_888 Image, as the HAL would actually hand it over.
 *
 * ImageFormat.YUV_420_888 names three planes, not a memory layout, and the shapes a legal HAL may
 * report for the chroma pair are more numerous than "NV12 or I420":
 *
 *   Planar          Cb and Cr are separate allocations, pixelStride 1, a chroma row is width/2 bytes.
 *                   This is what the emulator's HAL (sdk_gphone64_x86_64, Android 15) reports.
 *   SemiPlanarUV    ONE allocation holds Cb,Cr,Cb,Cr... planes[2] is planes[1]'s buffer offset by 1,
 *                   pixelStride 2. The shape the capture path originally assumed everybody was.
 *   SemiPlanarVU    ONE allocation holds Cr,Cb,Cr,Cb... so planes[1]'s buffer starts one byte LATE
 *                   relative to the pair. The U window is then not an NV12 plane at all: its odd
 *                   bytes are the NEXT column's Cr.
 *   SeparateStride2 Two allocations that each carry ONE component at pixelStride 2 spacing, so the
 *                   odd bytes of the Cb buffer are padding, not Cr.
 *
 * Android guarantees only that planes come Y,Cb,Cr in that order, that Y pixelStride is 1, and that
 * the U and V strides match — see https://developer.android.com/reference/android/graphics/ImageFormat#YUV_420_888
 * and https://developer.android.com/reference/android/media/Image.Plane#getBuffer() (the buffer
 * "may not include the trailing padding of the last row", which is why the limits below are tight
 * and why `uTrim` exists).
 *
 * EVERY byte in a fixture is position-dependent and component-specific:
 *   Cb samples land in 0x10..0x6F, Cr samples in 0x90..0xEF — disjoint ranges, so a packer that
 *   swaps the components cannot pass by luck — and both vary with row AND column, so a packer that
 *   walks a row with the wrong stride cannot pass either. Bytes that are NOT samples (row padding,
 *   the odd half of a stride-2 buffer, the tail past the last column) are 0xFF, which is in neither
 *   range: a packer that forwards them as if they were pixels is caught by the oracle, not by luck.
 *
 * The plane windows are allocated to EXACTLY their reported limit and not one byte more. That is
 * the whole reason an out-of-bounds read in the packer is an ASan report rather than a silent read
 * of slack, and it is what qa/build-host.sh's mp3 and mq mutants rely on to die by ASan instead of by
 * an assertion.
 */
#ifndef OMT_QA_FAKE_IMAGE_H
#define OMT_QA_FAKE_IMAGE_H

#include "nv12-oracle.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace fakehal {

enum class Shape { Planar, SemiPlanarUV, SemiPlanarVU, SeparateStride2 };

inline const char* shapeName(Shape s) {
    switch (s) {
        case Shape::Planar:          return "planar I420 (separate Cb/Cr, chroma pixelStride=1)";
        case Shape::SemiPlanarUV:    return "semi-planar NV12 (one UV allocation, Cb first, pixelStride=2)";
        case Shape::SemiPlanarVU:    return "semi-planar VU (one UV allocation, Cr first, pixelStride=2)";
        case Shape::SeparateStride2: return "separate stride-2 Cb/Cr allocations (pixelStride=2, not shared)";
    }
    return "?";
}

/** The bytes a plane's buffer would hold, as the HAL filled them. */
inline uint8_t cbValue(int r, int c) { return (uint8_t)(0x10 + ((r * 31 + c * 7) % 0x60)); }
inline uint8_t crValue(int r, int c) { return (uint8_t)(0x90 + ((r * 17 + c * 11) % 0x60)); }
constexpr uint8_t kNotASample = 0xFF;   // outside both sample ranges, so forwarding it is visible

struct FakeImage {
    const int w, h;
    const int yRow;              // luma rowStride; luma pixelStride is 1 by contract
    const int chromaRow;         // the U/V rowStride the HAL reports (the contract makes them equal)
    const Shape shape;

    /* The allocations. One for the luma plane; for the semi-planar shapes ONE shared chroma
     * allocation (that sharing is the whole point of those shapes); for the planar and
     * separate-stride-2 shapes two, which is what makes them distinguishable from the shared ones. */
    std::vector<uint8_t> y;
    std::vector<uint8_t> uv;     // semi-planar: the single CB_CR / CR_CB allocation
    std::vector<uint8_t> cB, cR; // planar and separate-stride-2: the two components

    /* Knobs a scenario turns to reproduce what a real HAL reports. */
    int64_t uTrim = 0;           // bytes shaved off Cb's reported limit (a view ending at its last sample)
    int64_t vTrim = 0;           // the same for Cr
    bool    chromaAllocPad = false; // the allocation carries one byte past the last sample of the row

    FakeImage(int width, int height, int lumaRowStride, int chromaRowStride, Shape s,
              bool chromaAllocationPad = false)
        : w(width), h(height), yRow(lumaRowStride), chromaRow(chromaRowStride), shape(s),
          chromaAllocPad(chromaAllocationPad) {
        const int rows = h / 2, cols = w / 2;
        y.assign((size_t)yRow * h, kNotASample);
        for (int r = 0; r < h; ++r)
            for (int c = 0; c < w; ++c) y[(size_t)r * yRow + c] = (uint8_t)(0x20 + ((r * 13 + c * 3) % 0xC0));

        /* Every row at the reported rowStride, the trailing padding of the last row included: that
         * is the region a HAL maps and reports as the plane. `chromaAllocationPad` adds the one byte
         * some devices leave after the final pair, and `uTrim`/`vTrim` pull the REPORTED limit back
         * from it — which is the difference between a plane that is a full NV12 UV region and one that
         * merely has pixelStride 2. */
        const size_t alloc = (size_t)chromaRow * (size_t)rows + (chromaAllocationPad ? 1 : 0);
        switch (s) {
            case Shape::SemiPlanarUV:
            case Shape::SemiPlanarVU: {
                uv.assign(alloc, kNotASample);
                for (int r = 0; r < rows; ++r)
                    for (int c = 0; c < cols; ++c) {
                        const size_t pair = (size_t)r * chromaRow + 2 * c;
                        if (s == Shape::SemiPlanarUV) { uv[pair] = cbValue(r, c); uv[pair + 1] = crValue(r, c); }
                        else                          { uv[pair] = crValue(r, c); uv[pair + 1] = cbValue(r, c); }
                    }
                break;
            }
            case Shape::SeparateStride2: {
                /* Two allocations, each carrying ONE component at stride-2 spacing. Nobody's odd
                 * byte is anybody else's sample, so a packer that copies the Cb buffer and calls the
                 * result NV12 puts 0xFF into every Cr slot. */
                cB.assign(alloc, kNotASample);
                cR.assign(alloc, kNotASample);
                for (int r = 0; r < rows; ++r)
                    for (int c = 0; c < cols; ++c) {
                        cB[(size_t)r * chromaRow + 2 * c] = cbValue(r, c);
                        cR[(size_t)r * chromaRow + 2 * c] = crValue(r, c);
                    }
                break;
            }
            case Shape::Planar: {
                cB.assign((size_t)chromaRow * rows, kNotASample);
                cR.assign((size_t)chromaRow * rows, kNotASample);
                for (int r = 0; r < rows; ++r)
                    for (int c = 0; c < cols; ++c) {
                        cB[(size_t)r * chromaRow + c] = cbValue(r, c);
                        cR[(size_t)r * chromaRow + c] = crValue(r, c);
                    }
                break;
            }
        }
    }

    /* ---- the descriptors Java would pass: (address, limit, rowStride, pixelStride) ----
     *
     * What Image.Plane.getBuffer() reports is the remainder of the plane's region from the plane's
     * offset, so on a shared allocation the Cb view is the whole UV region and the Cr view is that
     * minus the one-byte offset. `uTrim`/`vTrim` shave that report down to reproduce the HALs that
     * return a view ending at the plane's last own sample instead of at the end of the row — the
     * case that makes "the U buffer is an NV12 plane" false even when pixelStride says 2. */

    const uint8_t* uPtr() const {
        switch (shape) {
            case Shape::SemiPlanarUV: return uv.data();
            case Shape::SemiPlanarVU: return uv.data() + 1;          // Cb is the SECOND byte of the pair
            default:                  return cB.data();
        }
    }
    const uint8_t* vPtr() const {
        switch (shape) {
            case Shape::SemiPlanarUV: return uv.data() + 1;          // planes[2] aliases planes[1]
            case Shape::SemiPlanarVU: return uv.data();              // Cr is the FIRST byte of the pair
            default:                  return cR.data();
        }
    }
    int chromaPix() const { return shape == Shape::Planar ? 1 : 2; }

    /* Each plane's own allocation. GetDirectBufferCapacity() reports this, and it is the number the
     * JNI boundary compares the caller's claimed length against — a claim above it is a lie about
     * memory the callee is about to read. */
    size_t uAllocSize() const {
        return (shape == Shape::SemiPlanarUV || shape == Shape::SemiPlanarVU) ? uv.size()
             : (shape == Shape::SeparateStride2 || shape == Shape::Planar)     ? cB.size() : 0;
    }
    size_t vAllocSize() const {
        return (shape == Shape::SemiPlanarUV || shape == Shape::SemiPlanarVU) ? uv.size()
             : (shape == Shape::SeparateStride2 || shape == Shape::Planar)     ? cR.size() : 0;
    }
    size_t yAllocSize() const { return y.size(); }

    int64_t uLimit() const {
        const uint8_t* base = (shape == Shape::SemiPlanarUV || shape == Shape::SemiPlanarVU)
                                  ? uv.data() : cB.data();
        return (int64_t)uAllocSize() - (int64_t)(uPtr() - base) - uTrim;
    }
    int64_t vLimit() const {
        const uint8_t* base = (shape == Shape::SemiPlanarUV || shape == Shape::SemiPlanarVU)
                                  ? uv.data() : cR.data();
        return (int64_t)vAllocSize() - (int64_t)(vPtr() - base) - vTrim;
    }
    /* What GetDirectBufferCapacity() reports for the plane — the same region WITHOUT the caller's
     * trim, i.e. the truth the JNI boundary compares a claimed length against. A truthful caller
     * passes limit == capacity; an overstated one passes limit > capacity. */
    int64_t uCapacity() const { return uLimit() + uTrim; }
    int64_t vCapacity() const { return vLimit() + vTrim; }

    oracle::RawPlane planeU() const { return {uPtr(), uLimit(), chromaRow, chromaPix()}; }
    oracle::RawPlane planeV() const { return {vPtr(), vLimit(), chromaRow, chromaPix()}; }
    oracle::RawPlane planeY() const { return {y.data(), (int64_t)y.size(), yRow, 1}; }

    /* The pre-fix capture path's view of the same Image: it copied planes[0] and planes[1] back to
     * back and charged height*yStride + (height/2)*uvStride (app/src/main/cpp/OMTBridge.cpp:121).
     * Transcribed, signed int and all, because the number it produced is the evidence. */
    std::vector<uint8_t> legacyCopy() const {
        const oracle::RawPlane u = planeU();
        std::vector<uint8_t> buf(y.size() + (size_t)u.size);
        memcpy(buf.data(), y.data(), y.size());
        memcpy(buf.data() + y.size(), u.data, (size_t)u.size);
        return buf;
    }
    int legacyDataLength() const { return (int)(((h * yRow)) + ((h / 2) * chromaRow)); }

    /* A different frame must pack to different bytes; flipping luma bits is enough for every shape. */
    void shiftLuma() { for (auto& b : y) b ^= 0x5A; }
};

} // namespace fakehal

#endif /* OMT_QA_FAKE_IMAGE_H */
