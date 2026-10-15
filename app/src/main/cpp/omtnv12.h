/*
 * omtnv12.h — turn one Camera2 YUV_420_888 Image into the NV12 buffer this sender advertises.
 *
 * Why this file exists. ImageFormat.YUV_420_888 does NOT name a memory layout; it names a
 * *description* of three planes, and the HAL fills in that description. The chroma plane is legal
 * in two shapes, and Camera2 tells you which one you got with planes[1].getPixelStride():
 *
 *   pixelStride == 2  interleaved (NV12 / "CB_CR"): one buffer holds U,V,U,V,... A chroma ROW is
 *                     width bytes (width/2 pairs), and planes[2] aliases planes[1].
 *   pixelStride == 1  planar (I420): Cb and Cr are SEPARATE planes, one byte per sample, and a
 *                     chroma row is width/2 bytes. planes[1] and planes[2] are different buffers.
 *
 * The capture path used to assume the first shape unconditionally: it copied planes[1] verbatim
 * behind the luma plane and told the encoder that the result was NV12, charging the frame for
 * height*yStride + (height/2)*uvStride (app/src/main/cpp/OMTBridge.cpp:121). On a planar device the
 * HAL reports uv rowStride == width/2, so that arithmetic is half a chroma plane short — and the
 * value it produced was the number that got the stream refused:
 *
 *   E omt-sender: Sender::send: frame rejected: buffer too small for NV12 1920x1080 stride=1920:
 *   the encoder reads 3110400 bytes, dataLength is 2592000
 *
 * 2592000 == 1080*1920 + 540*960. 960 is width/2: the row length of a PLANAR chroma plane. So the
 * emulator's HAL (sdk_gphone64_x86_64, Android 15) reports pixelStride==1, every frame was rejected
 * by our own gate, and the wire carried a greeting, metadata, and zero video.
 *
 * What this does instead: it decodes whichever chroma layout the HAL reported and always writes the
 * SAME thing — a real NV12 buffer, UV interleaved, whose UV plane starts at exactly stride*height
 * and whose rows are all `stride` bytes. Sender::encodeFrame() hands VMX one stride for both planes
 * (OMTSender.cpp:374-377: uv = data + stride*height; VMX_EncodeNV12(y, stride, uv, stride)) and
 * validateFrame() charges the frame for stride*height*3/2, so writing anything else is either a
 * refusal or an out-of-bounds read. dataLength here is therefore stride*height*3/2 BY CONSTRUCTION,
 * not by arithmetic on whatever strides the HAL happened to report.
 *
 * The buffer we hand the encoder is ours, which also retires the old pathological case: a device
 * whose two row strides differ no longer walks the chroma plane with the luma stride, because the
 * copy reads each plane with ITS OWN rowStride and pads the output to the declared stride.
 *
 * Deliberately header-only and free of JNI/Android headers: qa/planar-chroma.cpp compiles this
 * exact file on the host against synthetic plane geometries, so the gate that decides whether a
 * phone can stream at all is not something only a phone can test.
 */
#ifndef OMT_BRIDGE_OMTNV12_H
#define OMT_BRIDGE_OMTNV12_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace omtpack {

/** One Camera2 plane as the HAL described it. `size` is the bytes the caller guarantees readable —
 *  Image.Plane.getBuffer().limit(), NOT the capacity of the whole Image allocation. */
struct Plane {
    const uint8_t* data = nullptr;
    int64_t size = 0;
    int rowStride = 0;
    int pixelStride = 0;
};

enum class Layout : int {
    Unknown = 0,
    InterleavedNV12 = 1,  // chroma pixelStride == 2 (CB_CR): planes[1] alone is the UV plane
    PlanarI420 = 2        // chroma pixelStride == 1: planes[1] = Cb, planes[2] = Cr, separate
};

struct Result {
    int stride = 0;          // the ONE stride to declare for both planes (the luma rowStride)
    int64_t dataLength = 0;  // == stride*height + (stride*height)/2, always
    Layout layout = Layout::Unknown;
};

inline const char* layoutName(Layout l) {
    return l == Layout::InterleavedNV12 ? "interleaved NV12 (chroma pixelStride=2)"
         : l == Layout::PlanarI420      ? "planar I420 (chroma pixelStride=1)"
                                        : "unknown";
}

namespace detail {
inline void setWhy(char* why, int cap, const char* msg) {
    if (why == nullptr || cap <= 0) return;
    std::snprintf(why, static_cast<size_t>(cap), "%s", msg);
}
} // namespace detail

/**
 * Pack one frame. Returns true and fills `out` when dst[0 .. out->dataLength) now holds NV12 the
 * encoder may read; returns false with a reason in `why` (never unreadable, never silent) and
 * leaves `out` zeroed. Nothing is read outside the (data, size) window of a plane, so a caller that
 * passes the plane buffers straight from Image.Plane.getBuffer() cannot make this over-read.
 */
inline bool packNv12(const Plane& y, const Plane& u, const Plane& v,
                     int width, int height,
                     uint8_t* dst, int64_t dstCapacity,
                     Result* out, char* why, int whyCap) {
    if (out != nullptr) *out = Result{};
    if (dst == nullptr) { detail::setWhy(why, whyCap, "null destination buffer"); return false; }
    if (out == nullptr) { detail::setWhy(why, whyCap, "null result"); return false; }
    if (width <= 0 || height <= 0) {
        detail::setWhy(why, whyCap, "non-positive frame dimensions");
        return false;
    }
    // Chroma is width/2 by height/2; an odd dimension would leave the last row or column without
    // chroma. validateFrame() refuses an odd NV12 height too — catching it here keeps the packer
    // from writing a half-populated plane before anybody asks.
    if ((width & 1) != 0 || (height & 1) != 0) {
        detail::setWhy(why, whyCap, "odd width/height cannot be subsampled 4:2:0");
        return false;
    }

    const int64_t w = width, h = height;
    const int64_t chromaRows = h / 2;
    const int64_t chromaCols = w / 2;

    /* ---- the luma plane: always 1 byte per sample, walked with its own rowStride ---- */
    if (y.data == nullptr) { detail::setWhy(why, whyCap, "null Y plane"); return false; }
    if (y.pixelStride != 1) {
        detail::setWhy(why, whyCap, "Y plane pixelStride is not 1 (not a YUV_420_888 luma plane)");
        return false;
    }
    if (y.rowStride < w) {
        detail::setWhy(why, whyCap, "Y rowStride cannot hold one luma row");
        return false;
    }
    const int64_t yNeed = (h - 1) * static_cast<int64_t>(y.rowStride) + w;
    if (y.size < yNeed) {
        detail::setWhy(why, whyCap, "Y plane buffer smaller than height*rowStride");
        return false;
    }

    /* ---- which chroma layout did the HAL report? This is the whole defect. ---- */
    const bool planar = (u.pixelStride == 1 && v.pixelStride == 1);
    const bool interleaved = (u.pixelStride == 2 && v.pixelStride == 2);
    if (!planar && !interleaved) {
        detail::setWhy(why, whyCap,
                       "unsupported chroma pixelStride (need 2 for NV12 or 1 for I420)");
        return false;
    }
    if (u.data == nullptr) { detail::setWhy(why, whyCap, "null chroma plane"); return false; }

    const int64_t stride = y.rowStride;  // the single stride the encoder will use for BOTH planes
    const int64_t dataLength = h * stride + chromaRows * stride;
    if (dstCapacity < dataLength) {
        detail::setWhy(why, whyCap, "destination buffer smaller than stride*height*3/2");
        return false;
    }

    if (interleaved) {
        // One chroma row = width bytes (width/2 U,V pairs). Anything narrower cannot hold the row,
        // and the check is what stops a malformed plane from being read past its end.
        if (u.rowStride < w) {
            detail::setWhy(why, whyCap, "NV12 chroma rowStride cannot hold one UV row");
            return false;
        }
        const int64_t uvNeed = (chromaRows - 1) * static_cast<int64_t>(u.rowStride) + w;
        if (u.size < uvNeed) {
            detail::setWhy(why, whyCap, "NV12 chroma buffer smaller than (height/2)*rowStride");
            return false;
        }
    } else {
        if (v.data == nullptr) { detail::setWhy(why, whyCap, "null Cr plane of a planar image"); return false; }
        if (u.rowStride < chromaCols || v.rowStride < chromaCols) {
            detail::setWhy(why, whyCap, "I420 chroma rowStride cannot hold one Cb/Cr row");
            return false;
        }
        const int64_t need = (chromaRows - 1) * static_cast<int64_t>(u.rowStride) + chromaCols;
        const int64_t needV = (chromaRows - 1) * static_cast<int64_t>(v.rowStride) + chromaCols;
        if (u.size < need || v.size < needV) {
            detail::setWhy(why, whyCap, "I420 chroma buffers smaller than (height/2)*rowStride");
            return false;
        }
    }

    /* ---- write it ---- */
    for (int64_t r = 0; r < h; ++r) {
        uint8_t* row = dst + r * stride;
        std::memcpy(row, y.data + r * static_cast<int64_t>(y.rowStride), static_cast<size_t>(w));
        if (stride > w) std::memset(row + w, 0, static_cast<size_t>(stride - w));
    }
    uint8_t* uv = dst + h * stride;  // where encodeFrame() will point VMX — do not move it
    if (interleaved) {
        for (int64_t r = 0; r < chromaRows; ++r) {
            uint8_t* row = uv + r * stride;
            std::memcpy(row, u.data + r * static_cast<int64_t>(u.rowStride), static_cast<size_t>(w));
            if (stride > w) std::memset(row + w, 0, static_cast<size_t>(stride - w));
        }
    } else {
        // The planar case has to INTERLEAVE: dst row is Cb0,Cr0,Cb1,Cr1,... A build that forgets
        // this line pair sends a chroma plane whose every second byte belongs to the wrong component.
        for (int64_t r = 0; r < chromaRows; ++r) {
            const uint8_t* srcU = u.data + r * static_cast<int64_t>(u.rowStride);
            const uint8_t* srcV = v.data + r * static_cast<int64_t>(v.rowStride);
            uint8_t* row = uv + r * stride;
            for (int64_t c = 0; c < chromaCols; ++c) {
                row[2 * c]     = srcU[c];
                row[2 * c + 1] = srcV[c];
            }
            if (stride > w) std::memset(row + w, 0, static_cast<size_t>(stride - w));
        }
    }

    out->stride = static_cast<int>(stride);
    out->dataLength = dataLength;
    out->layout = planar ? Layout::PlanarI420 : Layout::InterleavedNV12;
    return true;
}

/** Bytes of NV12 a caller must allocate for a plane geometry the packer may declare. Kept next to
 *  the packer so the Java side's allocate and this file's check cannot drift apart. */
inline int64_t nv12Size(int stride, int height) {
    if (stride <= 0 || height <= 0) return -1;
    const int64_t h = height, s = stride;
    return h * s + (h / 2) * s;
}

} // namespace omtpack

#endif // OMT_BRIDGE_OMTNV12_H
