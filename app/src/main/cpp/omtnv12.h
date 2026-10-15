/*
 * omtnv12.h — turn one Camera2 YUV_420_888 Image into the NV12 buffer this sender advertises.
 *
 * Why this file exists. ImageFormat.YUV_420_888 does NOT name a memory layout; it names a
 * *description* of three planes, and the HAL fills in that description. The chroma plane is legal
 * in more than one shape, and Camera2 tells you which one you got with planes[1].getPixelStride():
 *
 *   pixelStride == 1  planar (I420): Cb and Cr are SEPARATE planes, one byte per sample, and a
 *                     chroma row is width/2 bytes. planes[1] and planes[2] are different buffers.
 *   pixelStride == 2  semiplanar: one buffer holds the pairs, and planes[2] sits one byte inside
 *                     planes[1]'s — but WHICH byte of the pair is Cb is the HAL's choice, not the
 *                     format's (see planChroma below; NV12/CB_CR and NV21/CR_CB both exist).
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
 * Deliberately header-only and free of JNI/Android headers: qa/planar-chroma.cpp and
 * qa/jni-boundary.cpp compile this exact file on the host against synthetic plane geometries, so the
 * gate that decides whether a phone can stream at all is not something only a phone can test.
 */
#ifndef OMT_BRIDGE_OMTNV12_H
#define OMT_BRIDGE_OMTNV12_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>   // the refusal reasons are built, not hardcoded: a name that does not say WHICH
                    // plane was refused is the difference between a bug report and a phone call

namespace omtpack {

/** One Camera2 plane as the HAL described it. `size` is the bytes the caller guarantees readable —
 *  Image.Plane.getBuffer().limit(), NOT the capacity of the whole Image allocation. The JNI boundary
 *  in OMTBridge.cpp vetoes any claim that exceeds GetDirectBufferCapacity() of the buffer it came
 *  from before a Plane is constructed, because every bounds check in this file is of the form
 *  `size >= need` and trusts exactly that number (see admitSource). */
struct Plane {
    const uint8_t* data = nullptr;
    int64_t size = 0;
    int rowStride = 0;
    int pixelStride = 0;
};

/** The chroma layouts this path can honour, named by what the plane DESCRIPTORS say, not by what
 *  pixelStride alone suggests. InterleavedNV12 and SemiPlanarVU can report the same strides and
 *  differ only in which byte of the pair is Cb — which is why the packer must never decide "copy the
 *  U buffer" from pixelStride. */
enum class Layout : int {
    Unknown = 0,
    InterleavedNV12 = 1,  // one allocation, Cb at the even byte of each pair (CB_CR)
    PlanarI420 = 2,       // chroma pixelStride == 1: planes[1] = Cb, planes[2] = Cr, separate
    SemiPlanarVU = 3,     // one allocation, Cr at the even byte (CR_CB): the U window is NOT NV12
    SeparateStride2 = 4   // two allocations that each carry ONE component at pixelStride 2 spacing
};

struct Result {
    int stride = 0;          // the ONE stride to declare for both planes (the luma rowStride)
    int64_t dataLength = 0;  // == stride*height + (stride*height)/2, always
    Layout layout = Layout::Unknown;
};

inline const char* layoutName(Layout l) {
    switch (l) {
        case Layout::InterleavedNV12: return "interleaved NV12 / CB_CR (chroma pixelStride=2, Cb first)";
        case Layout::PlanarI420:      return "planar I420 (chroma pixelStride=1)";
        case Layout::SemiPlanarVU:    return "semiplanar VU / CR_CB (chroma pixelStride=2, Cr first)";
        case Layout::SeparateStride2: return "separate stride-2 Cb/Cr planes (chroma pixelStride=2, not shared)";
        case Layout::Unknown: break;
    }
    return "unknown";
}

namespace detail {
inline void setWhy(char* why, int cap, const char* msg) {
    if (why == nullptr || cap <= 0) return;
    std::snprintf(why, static_cast<size_t>(cap), "%s", msg);
}
} // namespace detail

/* ================================================================== *
 * THE LAYOUT DECISION
 * ================================================================== *
 * What Android actually guarantees about a YUV_420_888 Image is this, verbatim, from
 * https://developer.android.com/reference/android/graphics/ImageFormat#YUV_420_888 :
 *
 *   "Images in this format are always represented by three separate buffers of data, one for each
 *    color plane. Additional information always accompanies the buffers, describing the row stride
 *    and the pixel stride for each plane.
 *    The order of planes in the array returned by Image#getPlanes() is guaranteed such that plane
 *    #0 is always Y, plane #1 is always U (Cb), and plane #2 is always V (Cr).
 *    The Y-plane is guaranteed not to be interleaved with the U/V planes (in particular, pixel
 *    stride is always 1 in yPlane.getPixelStride()).
 *    The U/V planes are guaranteed to have the same row stride and pixel stride."
 *
 * and from https://developer.android.com/reference/android/media/Image.Plane#getBuffer() :
 *
 *   "each plane is only guaranteed to contain data up to the last pixel in the last row. In other
 *    words, the stride after the last row may not be mapped into the buffer."
 *
 * That is the entire contract, and it does NOT contain the sentence "a U plane whose pixelStride is
 * 2 holds a complete contiguous NV12 chroma plane". Three shapes the contract permits show the
 * difference, and the old code got all three wrong:
 *
 *   1. VU-backed semiplanar (CR_CB). The HAL's own plane offsets put Cb at byte 1 of each pair, so
 *      planes[2]'s address is one BELOW planes[1]'s. Copying the U window verbatim emits
 *      Cb0,Cr1,Cb1,Cr2,... — every Cr is its neighbour's, and the last column of each row reads the
 *      first byte of the NEXT row. The chroma is sheared, not merely mislabelled.
 *   2. Genuinely separate stride-2 Cb and Cr allocations. Each buffer holds ONE component at stride-2
 *      spacing, so the odd byte of the Cb buffer is padding the HAL never promised. Copying it into
 *      the Cr slots sends the HAL's slack bytes as chroma.
 *   3. A U view whose limit stops at its last Cb sample rather than at the end of the row — which the
 *      getBuffer() text above explicitly permits. Requiring the full width-byte row then rejects a
 *      perfectly valid frame, which is how a "robustness" check turns a working phone off the wire.
 *
 * So the layout is decided from the whole tuple the HAL actually reported — pixelStride, rowStride,
 * the buffer sizes, and the IDENTITY/OFFSET relationship between the U and V buffers — and the only
 * case that may memcpy the U window is the one where that tuple proves the window contains the whole
 * interleaved plane. Everything else samples Cb and Cr separately, each with its own descriptor,
 * which is the definition of a plane and needs no guessing at all.
 */
struct ChromaPlan {
    Layout layout = Layout::Unknown;
    bool verbatimU = false;   // the U window provably IS the interleaved plane: memcpy it
    Plane cb;                 // where Cb is read from — its own data/size/rowStride/pixelStride
    Plane cr;                 // where Cr is read from — likewise, never inferred from cb
};

/** The ONLY geometry in which the U buffer may be memcpy'd as if it were the NV12 plane itself: the
 *  caller's rowStride is at least one full interleaved row wide AND the reported window reaches the
 *  last byte of the last pair. A HAL that trims the view to its final Cb — which Image.Plane
 *  getBuffer() explicitly permits — fails this and is read with each plane's own descriptor instead,
 *  which costs a loop and produces the same bytes. Guessing it here is defect (1). */
inline bool uWindowIsTheWholePlane(const Plane& u, int64_t w, int64_t chromaRows) {
    return u.rowStride >= w &&
           u.size >= (chromaRows - 1) * static_cast<int64_t>(u.rowStride) + w;
}

/** Decide the chroma layout. Returns false with a named reason for geometry this path cannot honour;
 *  on success `plan` carries the two descriptors the writer will use. */
inline bool planChroma(const Plane& u, const Plane& v, int64_t w, int64_t chromaRows,
                       int64_t chromaCols, ChromaPlan* plan, char* why, int whyCap) {
    if (plan == nullptr) { detail::setWhy(why, whyCap, "null chroma plan"); return false; }
    *plan = ChromaPlan{};

    /* The contract guarantees the U and V PIXEL strides are equal. A HAL that reports them
     * differently is not describing either legal chroma layout, and guessing which half to believe is
     * how sheared colour reaches a viewer. (Row strides are a different matter: see the planar branch
     * below, which reads each plane with the rowStride it reported rather than demanding they match.) */
    if (u.pixelStride != v.pixelStride) {
        detail::setWhy(why, whyCap,
                       "chroma pixelStrides differ (Cb!=Cr) — Camera2 guarantees they are equal");
        return false;
    }
    if (u.pixelStride != 1 && u.pixelStride != 2) {
        detail::setWhy(why, whyCap,
                       "unsupported chroma pixelStride (need 2 for NV12 or 1 for I420)");
        return false;
    }
    if (chromaRows <= 0 || chromaCols <= 0) {
        detail::setWhy(why, whyCap, "chroma plane has no rows or columns");
        return false;
    }

    if (u.pixelStride == 1) {
        /* Planar I420: one byte per sample, Cb and Cr described separately, and each is read with
         * ITS OWN descriptor. Unlike the pixelStride==2 case below, a planar pair whose two buffers
         * turn out to be the same one is honoured rather than refused: sampling both components from
         * the bytes the HAL pointed at is the literal reading of the description, and a monochrome
         * sensor exposed as YUV_420_888 is exactly that shape. Refusing it would take a working
         * device off the wire to guard against a description that is merely unusual. (The stride-2
         * counterpart is NOT merely unusual — it asserts two components share one byte of one pair.)
         * Likewise the U and V row strides are used per plane even though the contract says they are
         * equal: if a HAL ever reports otherwise, the answer is still the frame it described. */
        plan->layout = Layout::PlanarI420;
        plan->cb = u;
        plan->cr = v;
        return true;
    }

    /* pixelStride == 2. The pairs are shared memory or they are not, and the only way to tell is to
     * look at where the two buffers actually ARE relative to each other. */
    const uintptr_t ua = reinterpret_cast<uintptr_t>(u.data);
    const uintptr_t va = reinterpret_cast<uintptr_t>(v.data);
    const int64_t off = (va >= ua) ? static_cast<int64_t>(va - ua)
                                   : -static_cast<int64_t>(ua - va);
    const bool overlap = (ua < va + static_cast<uintptr_t>(v.size)) &&
                         (va < ua + static_cast<uintptr_t>(u.size));

    if (!overlap) {
        /* Two allocations that each carry one component two bytes apart — and they provably ARE two
         * allocations, because distinct buffers cannot share a byte. Whether they happen to sit next
         * to each other in the heap is irrelevant: no byte belongs to both, so nothing may be copied
         * from one into the other. The odd byte of the Cb buffer belongs to no sample at all, and the
         * Cr plane is the only place Cr can be read from. */
        plan->layout = Layout::SeparateStride2;
        plan->cb = u;
        plan->cr = v;
        return true;
    }

    /* They share bytes, so this is ONE chroma allocation described twice. A single buffer has one row
     * length, so two different rowStrides here mean at least one of the descriptions is a lie. */
    if (u.rowStride != v.rowStride) {
        detail::setWhy(why, whyCap,
                       "chroma planes share one allocation but report different rowStrides");
        return false;
    }

    /* The pair order is the sign of the offset, and nothing else:
     *
     *   CB_CR (NV12): Cb at the even byte, planes[2] = planes[1] shifted +1.
     *   CR_CB (VU):   Cb at the odd byte, planes[2] = planes[1] shifted -1 — and copying the Cb window
     *                 in that shape emits Cb0,Cr1,Cb1,Cr2,..., shearing every chroma row and reading
     *                 the NEXT row's first byte where the last Cr should be.
     */
    if (off == 1) {
        plan->layout = Layout::InterleavedNV12;
        plan->cb = u;
        plan->cr = v;
        plan->verbatimU = uWindowIsTheWholePlane(u, w, chromaRows);
        return true;
    }
    if (off == -1) {
        plan->layout = Layout::SemiPlanarVU;
        plan->cb = u;   // MQ2 anchor: exchanging these two lines is the VU ordering defect — Cb is the
        plan->cr = v;   // byte the HAL pointed planes[1] at, whichever half of the pair that happens to be
        plan->verbatimU = false;   // the U window's odd bytes are the NEIGHBOURING column's Cr
        return true;
    }
    if (off == 0) {
        /* "Three separate buffers, one for each color plane" means the Cb buffer holds Cb and the Cr
         * buffer holds Cr. Two planes at the SAME address with the SAME spacing assert that every Cr
         * equals the Cb beside it, which no camera means; reading them apart would be a guess about
         * which half of the pair the HAL forgot to offset. Refuse and name it, so the log says which
         * device does it — the alternative is a stream with the colour channels folded together. */
        detail::setWhy(why, whyCap,
                       "Cb and Cr planes report the same base address at pixelStride 2 (malformed)");
        return false;
    }
    detail::setWhy(why, whyCap, "chroma planes overlap at an offset that is not a Cb/Cr pair");
    return false;
}

/**
 * Pack one frame. Returns true and fills `out` when dst[0 .. out->dataLength) now holds NV12 the
 * encoder may read; returns false with a reason in `why` (never unreadable, never silent) and
 * leaves `out` zeroed. Nothing is read outside the (data, size) window of a plane, so a caller that
 * passes the plane buffers straight from Image.Plane.getBuffer() cannot make this over-read — as
 * long as `size` is the truth about that buffer, which is what admitSource() is for.
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
    ChromaPlan plan;
    if (!planChroma(u, v, w, chromaRows, chromaCols, &plan, why, whyCap)) return false;

    const int64_t stride = y.rowStride;  // the single stride the encoder will use for BOTH planes
    const int64_t dataLength = h * stride + chromaRows * stride;
    if (dstCapacity < dataLength) {
        detail::setWhy(why, whyCap, "destination buffer smaller than stride*height*3/2");
        return false;
    }

    /* ---- bound each component by ITS OWN descriptor, not by the U plane's ----
     * The last byte a component may be read at is (rows-1)*rowStride + (cols-1)*pixelStride. Note the
     * trailing `+1`, not `+pixelStride`: Image.Plane.getBuffer() only guarantees data up to the last
     * SAMPLE, and a HAL is allowed to stop its view there. Asking for the rest of the row would
     * reject a valid frame to satisfy a check that was never in the contract. */
    struct Bound { const char* name; const Plane* p; };
    const Bound bounds[2] = { { "Cb", &plan.cb }, { "Cr", &plan.cr } };
    for (const Bound& b : bounds) {
        if (b.p->data == nullptr) {
            detail::setWhy(why, whyCap,
                           (std::string(b.name) + " chroma plane is null").c_str());
            return false;
        }
        const int64_t rowBytes = (chromaCols - 1) * static_cast<int64_t>(b.p->pixelStride) + 1;
        if (b.p->rowStride < rowBytes) {
            detail::setWhy(why, whyCap, "chroma rowStride cannot hold one Cb/Cr row");
            return false;
        }
        const int64_t need = (chromaRows - 1) * static_cast<int64_t>(b.p->rowStride) + rowBytes;
        if (b.p->size < need) {
            detail::setWhy(why, whyCap,
                           (std::string(b.name) + " chroma buffer smaller than (height/2)*rowStride").c_str());
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
    if (plan.verbatimU) {
        /* The one case where the U buffer IS an NV12 plane, proven by planChroma: same allocation as
         * Cr, Cr immediately after Cb, matching row strides, and a window that reaches the last byte
         * of the last pair. Anything else lands in the else branch below. */
        for (int64_t r = 0; r < chromaRows; ++r) {
            uint8_t* row = uv + r * stride;
            std::memcpy(row, plan.cb.data + r * static_cast<int64_t>(plan.cb.rowStride),
                        static_cast<size_t>(w));
            if (stride > w) std::memset(row + w, 0, static_cast<size_t>(stride - w));
        }
    } else {
        /* Sample each component from its own plane, at its own spacing. This covers planar I420,
         * VU-backed semiplanar, separate stride-2 buffers, and a trimmed Cb view — four shapes that
         * look identical to a pixelStride test and produce four different byte streams if you guess.
         * A build that forgets to interleave here sends a chroma plane whose every second byte
         * belongs to the wrong component. */
        for (int64_t r = 0; r < chromaRows; ++r) {
            const uint8_t* srcU = plan.cb.data + r * static_cast<int64_t>(plan.cb.rowStride);
            const uint8_t* srcV = plan.cr.data + r * static_cast<int64_t>(plan.cr.rowStride);
            uint8_t* row = uv + r * stride;
            for (int64_t c = 0; c < chromaCols; ++c) {
                row[2 * c]     = srcU[c * plan.cb.pixelStride];
                row[2 * c + 1] = srcV[c * plan.cr.pixelStride];
            }
            if (stride > w) std::memset(row + w, 0, static_cast<size_t>(stride - w));
        }
    }

    out->stride = static_cast<int>(stride);
    out->dataLength = dataLength;
    out->layout = plan.layout;
    return true;
}

/* ================================================================== *
 * THE JNI BOUNDARY'S OTHER HALF
 * ================================================================== *
 * A direct ByteBuffer comes with two numbers about how much of it exists: the limit the caller
 * passes as an argument, and the capacity the JVM reports for the buffer object. Only one of them is
 * not a claim. Every bounds check in packNv12() above reads `plane.size`, so a caller that overstates
 * a size — a Java side that computed the length from its own geometry instead of getBuffer().limit(),
 * a buffer that was resized after the frame was acquired, a future refactor — silently disables all
 * of them and the packer walks off the end of the plane allocation.
 *
 * So the bridge asks the buffer, not the caller: capacity must cover the claim, and a length cannot
 * be negative. This lives next to the packer, and is header-only like it, so qa/jni-boundary.cpp can
 * run the real production entry point against capacities that do not match its claims — and so that
 * deleting the check is an ASan heap-buffer-overflow rather than a code review finding.
 */
struct SourceClaim {
    const uint8_t* data = nullptr;
    int64_t claimed = 0;     // the *Size argument Java supplied
    int64_t capacity = 0;    // GetDirectBufferCapacity() of the buffer that argument came from
};

inline bool admitSource(const SourceClaim& c, const char* what, char* why, int whyCap) {
    if (c.data == nullptr) {
        detail::setWhy(why, whyCap, "null plane buffer address");
        return false;
    }
    if (c.capacity < 0) {   // GetDirectBufferCapacity returns -1 for a buffer that is not direct
        detail::setWhy(why, whyCap,
                       (std::string(what) + " is not a direct buffer (capacity " +
                        std::to_string(c.capacity) + ")").c_str());
        return false;
    }
    if (c.claimed < 0) {
        detail::setWhy(why, whyCap,
                       (std::string(what) + " claims " + std::to_string(c.claimed) +
                        " readable bytes; a length cannot be negative (direct buffer capacity is " +
                        std::to_string(c.capacity) + ")").c_str());
        return false;
    }
    if (c.claimed > c.capacity) {
        detail::setWhy(why, whyCap,
                       (std::string(what) + " claims " + std::to_string(c.claimed) +
                        " readable bytes but its direct buffer capacity is only " +
                        std::to_string(c.capacity) + " — refusing before the first read").c_str());
        return false;
    }
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
