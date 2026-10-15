/*
 * nv12-oracle.h — TEST-ONLY. The expected NV12 bytes, built from the plane descriptors alone.
 *
 * Why a separate file, and why it is written the way it is:
 *
 * The suite has to answer "did the packer produce the chroma the camera meant?", and the only
 * acceptable evidence is a comparison against something that does not share code with the packer.
 * If the assertion calls omtpack, or copies omtpack's branch structure, or asks the packer what it
 * decided and then checks that it decided that, the test is vacuous: it passes for the same reason
 * the production code passes, whatever that is.
 *
 * So this file contains the *definition* and nothing else. Android's YUV_420_888 contract says a
 * plane is walked as
 *
 *     sample(row, col) = plane.getBuffer()[ row * plane.getRowStride()
 *                                         + col * plane.getPixelStride() ]
 *
 * for each of the three planes independently, with Cb = planes[1] and Cr = planes[2] regardless of
 * how the HAL laid the bytes out. NV12 is then, by construction,
 *
 *     dst[r*stride + c]                 = Y.sample(r, c)          stride = Y.rowStride
 *     dst[stride*h + r*stride + 2*c]    = Cb.sample(r, c)
 *     dst[stride*h + r*stride + 2*c + 1]= Cr.sample(r, c)
 *
 * There is no `if (pixelStride == 2)` here, because the definition does not need one: the stride IS
 * the layout. That is what makes this an oracle rather than a second opinion — a packer that guesses
 * the layout from one field and then special-cases the copy can only agree with it by accident, and
 * the mutants in qa/build-host.sh exist to show this suite does not accept accidents.
 *
 * Every read is checked against the plane's own reported size before it happens. If a fixture asks
 * the oracle to read past a plane's limit, that is a broken fixture, and it comes back as
 * `harnessError` — not as a product failure, and not as a silently tolerated garbage byte.
 *
 * Production code must never include this file. It lives under qa/ and nothing in app/ or libomt/
 * references it.
 */
#ifndef OMT_QA_NV12_ORACLE_H
#define OMT_QA_NV12_ORACLE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oracle {

/** One plane exactly as Image.Plane describes it: a window into somebody's allocation, plus the two
 *  strides. Deliberately NOT omtpack::Plane — the oracle must not depend on the type under test. */
struct RawPlane {
    const uint8_t* data = nullptr;
    int64_t size = 0;
    int rowStride = 0;
    int pixelStride = 0;
};

struct Expected {
    bool harnessError = false;     // the FIXTURE is wrong (a read outside a plane's own window)
    std::string why;
    int stride = 0;
    int64_t dataLength = 0;
    std::vector<uint8_t> bytes;    // stride*height*3/2 bytes, padding zeroed
};

inline Expected nv12(const RawPlane& y, const RawPlane& u, const RawPlane& v, int width, int height) {
    Expected e;
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
        e.harnessError = true; e.why = "the fixture asked for a frame this format cannot describe";
        return e;
    }
    if (!y.data || !u.data || !v.data) {
        e.harnessError = true; e.why = "the fixture handed the oracle a null plane"; return e;
    }
    if (y.pixelStride != 1) {
        e.harnessError = true; e.why = "Y pixelStride is not 1 — not a YUV_420_888 luma plane"; return e;
    }
    const int64_t w = width, h = height, stride = y.rowStride;
    const int64_t cRows = h / 2, cCols = w / 2;
    e.stride = (int)stride;
    e.dataLength = stride * h + stride * cRows;
    e.bytes.assign((size_t)e.dataLength, 0);

    auto read = [&](const char* who, const RawPlane& p, int64_t row, int64_t col, uint8_t* out) -> bool {
        const int64_t off = row * (int64_t)p.rowStride + col * (int64_t)p.pixelStride;
        if (off < 0 || off >= p.size) {
            e.harnessError = true;
            e.why = std::string("the fixture asks the oracle to read ") + who +
                    " at row " + std::to_string(row) + " col " + std::to_string(col) +
                    " (offset " + std::to_string(off) + ") outside its own reported size " +
                    std::to_string(p.size) + " — a broken fixture, not a product failure";
            return false;
        }
        *out = p.data[off];
        return true;
    };

    for (int64_t r = 0; r < h; ++r)
        for (int64_t c = 0; c < w; ++c) {
            uint8_t b = 0;
            if (!read("Y", y, r, c, &b)) { e.bytes.clear(); return e; }
            e.bytes[(size_t)(r * stride + c)] = b;      /* c < w <= stride: the tail stays zero */
        }
    for (int64_t r = 0; r < cRows; ++r)
        for (int64_t c = 0; c < cCols; ++c) {
            uint8_t cu = 0, cv = 0;
            if (!read("Cb", u, r, c, &cu)) { e.bytes.clear(); return e; }
            if (!read("Cr", v, r, c, &cv)) { e.bytes.clear(); return e; }
            const size_t at = (size_t)(stride * h + r * stride + 2 * c);
            e.bytes[at] = cu;
            e.bytes[at + 1] = cv;
        }
    return e;
}

/** First differing byte between what the packer wrote and what the oracle expects, or -1. */
inline long long firstMismatch(const std::vector<uint8_t>& got, const std::vector<uint8_t>& want) {
    const size_t n = std::min(got.size(), want.size());
    for (size_t i = 0; i < n; ++i) if (got[i] != want[i]) return (long long)i;
    if (got.size() != want.size()) return (long long)n;
    return -1;
}

} // namespace oracle

#endif /* OMT_QA_NV12_ORACLE_H */
