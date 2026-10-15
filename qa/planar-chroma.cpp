/*
 * planar-chroma.cpp — the capture path must honour whichever chroma layout the HAL reported.
 *
 * ImageFormat.YUV_420_888 does not name a memory layout. It names three planes, and the HAL fills
 * in the description; planes[1].getPixelStride() is the only thing that says which chroma you got:
 *
 *   pixelStride == 2  interleaved (NV12/CB_CR): one buffer, Cb,Cr,Cb,Cr..., a chroma row is width
 *                     bytes and planes[2] aliases planes[1].
 *   pixelStride == 1  planar (I420): Cb and Cr are SEPARATE planes, a chroma row is width/2 bytes.
 *
 * The capture path assumed the first shape. It copied planes[1] verbatim behind the luma plane,
 * then charged the frame for height*yStride + (height/2)*uvStride (app/src/main/cpp/OMTBridge.cpp:121
 * before this fix) while the encoder walks BOTH planes at the single declared stride
 * (OMTSender.cpp:374-377 — uv = data + stride*height; VMX_EncodeNV12(y, stride, uv, stride)). On a
 * planar device uv rowStride is width/2, so the frame came up half a chroma plane short and
 * Sender::validateFrame() — correctly — refused every single one:
 *
 *   E omt-sender: Sender::send: frame rejected: buffer too small for NV12 1920x1080 stride=1920:
 *   the encoder reads 3110400 bytes, dataLength is 2592000
 *
 * measured on emulator-5554 (Android 15, sdk_gphone64_x86_64), whose back camera reports exactly
 * y rowStride=1920 pixelStride=1, chroma rowStride=960 pixelStride=1: 2592000 = 1080*1920 + 540*960.
 * The wire carried the greeting and the metadata and zero video frames.
 *
 * WHAT THIS PROVES, and what each binary in qa/build-host.sh's planar section is for:
 *
 *   pc-fixed   — omtpack::packNv12() (app/src/main/cpp/omtnv12.h, the SAME header the APK compiles)
 *                turns each synthetic plane geometry into a buffer that (a) survives the sender's
 *                gate unchanged, (b) really encodes to a subscriber, and (c) contains the bytes the
 *                camera meant: every luma row verbatim, and every chroma row the true Cb,Cr
 *                interleaving — not the U plane twice, not the chroma walked at the luma stride.
 *   pc-prefix  — the same scenarios against a verbatim transcription of the pre-fix capture path
 *                (Java copy + OMTBridge.cpp:121 arithmetic). Case B is the emulator's geometry and
 *                MUST go red there, with the 3110400-vs-2592000 refusal named in the output. If
 *                this binary ever comes back green, the harness has stopped measuring the defect.
 *   mutants    — omtnv12.h with one property sabotaged each (ignore pixelStride, forget to
 *                interleave, walk the chroma row with the luma stride, charge the old dataLength).
 *                Each must die; the third dies to ASan, which is the whole reason the planes are
 *                allocated to their exact plane size and not one byte more.
 *
 * Android host QA is ASan-only here (no libubsan for this target — see qa/build-host.sh), so an
 * out-of-bounds READ is a crash and a crash is the verdict.
 *
 * Build + run: bash qa/build-host.sh   (which also runs the pre-fix and mutant controls)
 */
#include "OMTSender.h"
#include "OMTProtocol.h"
#include "libomt.h"
#include "omtnv12.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace omt;

static int failures = 0;

static void ok(bool cond, const std::string& what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++failures;
}
static bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

/* ------------------------------------------------------------------ *
 * A Camera2 Image, as the HAL would hand it over.
 *
 * Each plane is its OWN allocation sized exactly what Image.Plane.getBuffer().limit() reports —
 * no slack. That is the point: the pre-fix code walked the chroma plane with the luma stride, and
 * the only honest way to show it did so is to let ASan see the end of the plane.
 * ------------------------------------------------------------------ */
struct FakeImage {
    int w, h;
    int yRow, yPix, uRow, uPix, vRow, vPix;
    std::vector<uint8_t> y, u, v;   // v aliases u for the interleaved layouts
    const bool interleavedSource;

    FakeImage(int width, int height, int yRowStride, int chromaRowStride, bool interleaved)
        : w(width), h(height),
          yRow(yRowStride), yPix(1),
          uRow(chromaRowStride), uPix(interleaved ? 2 : 1),
          vRow(chromaRowStride), vPix(interleaved ? 2 : 1),
          interleavedSource(interleaved) {
        y.resize((size_t)yRow * h);
        if (interleaved) {
            u.resize((size_t)chromaRowStride * (h / 2));   // one CB_CR plane
        } else {
            u.resize((size_t)chromaRowStride * (h / 2));   // separate Cb
            v.resize((size_t)chromaRowStride * (h / 2));   // separate Cr
        }
        // Deterministic, position-dependent patterns. Cb and Cr differ in EVERY byte: a packer that
        // forgot to interleave, or swapped the two components, cannot survive the content checks.
        uint32_t rng = 0xB16B00B5u ^ (uint32_t)(w * 7919 + h);
        for (auto& b : y) { rng = rng * 1103515245u + 12345u; b = (uint8_t)(rng >> 16); }
        if (interleaved) {
            for (size_t i = 0; i < u.size(); ++i)
                u[i] = (i & 1) ? (uint8_t)(0x90 + ((i / 2) % 0x60))   // Cr
                               : (uint8_t)(0x10 + ((i / 2) % 0x60));  // Cb
            v = u;   // planes[2] aliases planes[1] on a CB_CR device
        } else {
            for (size_t i = 0; i < u.size(); ++i) u[i] = (uint8_t)(0x10 + (i % 0x60));
            for (size_t i = 0; i < v.size(); ++i) v[i] = (uint8_t)(0x90 + (i % 0x60));
        }
        // Padding beyond the visible row is deliberately 0xFF: it must NOT reach the encoder's view
        // of the visible pixels, and the packed output's own padding is asserted to be zero.
        for (int r = 0; r < h; ++r)
            for (int c = w; c < yRow; ++c) y[(size_t)r * yRow + c] = 0xFF;
        if (interleaved) {
            for (int r = 0; r < h / 2; ++r)
                for (int c = w; c < chromaRowStride; ++c) u[(size_t)r * chromaRowStride + c] = 0xFF;
        } else {
            for (int r = 0; r < h / 2; ++r)
                for (int c = w / 2; c < chromaRowStride; ++c) {
                    u[(size_t)r * chromaRowStride + c] = 0xFF;
                    v[(size_t)r * chromaRowStride + c] = 0xFF;
                }
        }
    }

    omtpack::Plane planeY() const { return {y.data(),   (int64_t)y.size(), yRow, yPix}; }
    omtpack::Plane planeU() const { return {u.data(),   (int64_t)u.size(), uRow, uPix}; }
    omtpack::Plane planeV() const { return {v.data(),   (int64_t)v.size(), vRow, vPix}; }

    /* The pre-fix capture path, transcribed: OMTStreamingManager.sendFrame() copied planes[0] and
     * planes[1] back to back into one direct buffer, and OMTBridge.cpp:121 charged
     *     int dataLength = (height * yStride) + ((height / 2) * uvStride);
     * with Stride = yStride. Reproduced here verbatim, including the signed int. */
    std::vector<uint8_t> legacyCopy() const {
        std::vector<uint8_t> buf(y.size() + u.size());
        memcpy(buf.data(), y.data(), y.size());
        memcpy(buf.data() + y.size(), u.data(), u.size());
        return buf;
    }
    int legacyDataLength() const {
        return (int)(((h * yRow)) + ((h / 2) * uRow));
    }
};

static MediaFrame frameOf(void* data, int w, int h, int stride, int dataLength) {
    MediaFrame f{};
    f.data = data;
    f.dataLength = dataLength;
    f.width = w;
    f.height = h;
    f.stride = stride;
    f.codec = Codec::NV12;
    f.frameRateN = 30;
    f.frameRateD = 1;
    f.aspectRatio = (float)w / (float)h;
    f.flags = 0;                       // opaque: no Alpha bit, see the wire header assertions
    f.colorSpace = (h >= 720) ? ColorSpace::BT709 : ColorSpace::BT601;
    f.timestamp = 0;
    return f;
}

/* One frame, from whichever builder is under test, plus everything the assertions need. */
struct Built {
    std::vector<uint8_t> buf;
    int stride = 0;
    int dataLength = 0;
    bool packed = false;
    std::string why;
};

/* Build the frame the way the code under test would. Fixed = the packer; prefix = the transcription
 * of the capture path as it stood before the fix, from the same plane geometry. */
static Built build(const FakeImage& img, int dstSlop) {
    Built b;
#ifdef OMT_QA_PLANAR_PREFIX
    (void)dstSlop;
    b.buf = img.legacyCopy();
    b.stride = img.yRow;
    b.dataLength = img.legacyDataLength();
    b.packed = true;   // "the builder ran", not "the result is NV12" — the assertions decide that
    return b;
#else
    const int64_t need = omtpack::nv12Size(img.yRow, img.h);
    if (need < 0) { b.why = "nv12Size rejected the geometry"; return b; }
    b.buf.assign((size_t)(need + dstSlop), 0xA5);   // 0xA5: never a plausible pixel value
    omtpack::Result r{};
    char why[192] = {};
    omtpack::Plane y = img.planeY(), u = img.planeU(), v = img.planeV();
    b.packed = omtpack::packNv12(y, u, v, img.w, img.h, b.buf.data(), need + dstSlop, &r,
                                why, (int)sizeof(why));
    b.stride = r.stride;
    b.dataLength = (int)r.dataLength;
    b.why = why;
    return b;
#endif
}

/* The assertions every layout must satisfy, written once so the fixed and pre-fix binaries are
 * measured against the identical bar. */
static void expectGoodNv12(const Built& b, const FakeImage& img, const std::string& tag) {
    std::string why;
    MediaFrame f = frameOf(b.buf.empty() ? nullptr : (void*)b.buf.data(), img.w, img.h, b.stride,
                           b.dataLength);
    if (!b.packed) {
        ok(false, tag + ": the packer refused the frame — " + b.why);
        return;
    }

    const long long wantLength = (long long)b.stride * img.h * 3 / 2;
    ok(b.dataLength == wantLength,
       tag + ": dataLength is exactly stride*height*3/2 (" + std::to_string(b.dataLength)
           + " vs " + std::to_string(wantLength) + ") — the number the encoder is documented to read");
    ok((long long)b.buf.size() >= wantLength,
       tag + ": the buffer we hand over is at least that long, so the claim is backed by bytes");

    why.clear();
    bool valid = Sender::validateFrameForTest(f, why);
    ok(valid, tag + ": the sender's gate accepts it as built" + (valid ? std::string()
        : std::string("   [refused: \"") + why + "\"]"));

    /* Content: luma rows verbatim (read with the SOURCE rowStride, not the destination's). */
    bool yOk = true;
    for (int r = 0; r < img.h && yOk; ++r)
        for (int c = 0; c < img.w; ++c)
            if (b.buf[(size_t)r * b.stride + c] != img.y[(size_t)r * img.yRow + c]) { yOk = false; break; }
    ok(yOk, tag + ": every luma row is the camera's row, copied with the Y plane's own rowStride");

#ifndef OMT_QA_PLANAR_PREFIX
    // A property of the packer, not of the defect: the pre-fix path forwarded the HAL's padding
    // bytes as they were. Asserting it in both builds would make pc-prefix red for reasons that
    // are not the bug, and a harness that is red everywhere proves nothing.
    bool padOk = true;
    for (int r = 0; r < img.h && padOk; ++r)
        for (int c = img.w; c < b.stride; ++c)
            if (b.buf[(size_t)r * b.stride + c] != 0) { padOk = false; break; }
    ok(padOk, tag + ": luma padding past the visible row is zeroed, not whatever the HAL left there");
#endif

    /* Content: the chroma rows. This is the assertion that distinguishes a real NV12 buffer from a
     * planar Cb plane wearing NV12's clothes. */
    const uint8_t* uv = b.buf.data() + (size_t)b.stride * img.h;
    bool uvOk = true;
    long long badAt = -1;
    for (int r = 0; r < img.h / 2 && uvOk; ++r) {
        for (int c = 0; c < img.w / 2; ++c) {
            const uint8_t wantU = img.interleavedSource
                    ? img.u[(size_t)r * img.uRow + 2 * c]
                    : img.u[(size_t)r * img.uRow + c];
            const uint8_t wantV = img.interleavedSource
                    ? img.u[(size_t)r * img.uRow + 2 * c + 1]
                    : img.v[(size_t)r * img.vRow + c];
            if (uv[(size_t)r * b.stride + 2 * c] != wantU ||
                uv[(size_t)r * b.stride + 2 * c + 1] != wantV) {
                uvOk = false; badAt = (long long)r * img.w + c; break;
            }
        }
    }
    ok(uvOk, tag + ": every chroma row is the true Cb,Cr interleaving"
             + (uvOk ? std::string() : "   [first mismatch at chroma pixel " + std::to_string(badAt)
                     + " — the plane was copied, walked with the wrong stride, or not interleaved]"));

#ifndef OMT_QA_PLANAR_PREFIX
    bool uvPadOk = true;
    for (int r = 0; r < img.h / 2 && uvPadOk; ++r)
        for (int c = img.w; c < b.stride; ++c)
            if (uv[(size_t)r * b.stride + c] != 0) { uvPadOk = false; break; }
    ok(uvPadOk, tag + ": chroma padding is zeroed too, so nothing outside the visible plane is sent");
#endif
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
#ifdef OMT_QA_PLANAR_PREFIX
    std::printf("planar-chroma: PRE-FIX capture path (OMTBridge.cpp:121 arithmetic + the Java plane copy)\n");
#else
    std::printf("planar-chroma: omtpack::packNv12 (app/src/main/cpp/omtnv12.h)\n");
#endif

    /* A live sender: the gate is not the last word — a frame that passes it must also ENCODE and
     * reach a subscriber, or the harness would be scoring a gate that accepts garbage. */
    SenderConfig cfg;
    cfg.name = "qa-planar-chroma";
    cfg.port = 45341;
    cfg.quality = Quality::Default;
    Sender sender(cfg);
    if (!sender.start()) {
        std::printf("  [FAIL] sender.start() on port %d\n", cfg.port);
        return 1;
    }
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(cfg.port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    ok(::connect(fd, (sockaddr*)&a, sizeof(a)) == 0, "a subscriber socket connected");
    for (int i = 0; i < 100 && sender.connectionCount() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ok(sender.connectionCount() == 1, "the sender accepted it as a channel");
    {   // Without the subscribe control frame Sender::send skips the channel and nothing encodes.
        const char* xml = MetadataConstants::CHANNEL_SUBSCRIBE_VIDEO;
        const size_t xlen = strlen(xml) + 1;
        std::vector<uint8_t> m(FrameHeader::SIZE + xlen, 0);
        FrameHeader h{};
        h.version = 1; h.frameType = static_cast<uint8_t>(FrameType::Metadata);
        h.timestamp = 0; h.metadataLength = (uint16_t)xlen; h.dataLength = (int32_t)xlen;
        memcpy(m.data(), &h, sizeof(h));
        memcpy(m.data() + FrameHeader::SIZE, xml, xlen);
        ok(::send(fd, m.data(), m.size(), MSG_NOSIGNAL) == (ssize_t)m.size(),
           "the subscriber sent the OMT video-subscribe control frame");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    struct Case {
        const char* name;
        int w, h, yRow, chromaRow;
        bool interleaved;
    };
    const Case cases[] = {
        // (a) the layout the code assumed, and the one real phones with NV12 HALs still report.
        //     yStride == uvStride: this is the shape that must keep working exactly as it did.
        {"A interleaved NV12, no padding",       256,  64, 256, 256, true},
        {"A2 interleaved NV12, padded luma",     256,  64, 320, 320, true},
        // (b) THE EMULATOR. 1920x1080, y rowStride 1920, chroma rowStride 960 = width/2, chroma
        //     pixelStride 1 — measured off emulator-5554, and the geometry that produced
        //     "the encoder reads 3110400 bytes, dataLength is 2592000" on every frame.
        {"B planar I420, emulator geometry",     1920, 1080, 1920, 960, false},
        {"B2 planar I420, small",                256,  64, 256, 128, false},
        {"B3 planar I420, padded luma",          256,  64, 320, 160, false},
        // (c) the pathological stride neighbourhood the gate exists for, in both directions. C1 is
        //     the case qa/frame-validation.cpp A21 refuses; C2 has a chroma row WIDER than the
        //     luma row, which the old code copied whole and the encoder then walked at yStride —
        //     the picture comes out sheared, and only a content assertion can see it.
        {"C1 interleaved, uv rowStride < y rowStride", 256, 64, 320, 256, true},
        {"C2 interleaved, uv rowStride > y rowStride", 256, 64, 256, 384, true},
        {"C3 planar, chroma row wider than luma row",  256, 64, 256, 200, false},
    };

    for (const auto& c : cases) {
        FakeImage img(c.w, c.h, c.yRow, c.chromaRow, c.interleaved);
        std::printf("  -- %s: %dx%d y rowStride=%d, chroma rowStride=%d pixelStride=%d\n",
                    c.name, c.w, c.h, c.yRow, c.chromaRow, c.interleaved ? 2 : 1);
        Built b = build(img, 0);
        expectGoodNv12(b, img, c.name);

        // It must actually encode and reach the subscriber, not merely survive the predicate.
        const long long before = sender.encodeFailures();
        MediaFrame f = frameOf(b.buf.empty() ? nullptr : (void*)b.buf.data(), c.w, c.h, b.stride,
                               b.dataLength);
        int r = 0;
        bool survived = true;
        try { r = sender.send(f); } catch (...) { survived = false; }
        ok(survived && r > 0, std::string(c.name) + ": the frame encodes and reaches the subscriber"
                              " (returned " + std::to_string(r) + " bytes)"
                              + (survived ? std::string() : std::string("   [THREW — the buffer was not readable]")));
        ok(sender.encodeFailures() == before, std::string(c.name)
             + ": and is not counted as an encode failure, so the counter keeps meaning what it says");

        // A packer that read the same plane twice would pass every check above on a static source.
        FakeImage moved(c.w, c.h, c.yRow, c.chromaRow, c.interleaved);
        for (auto& byte : moved.y) byte ^= 0x5A;
        Built b2 = build(moved, 0);
        ok(b2.buf != b.buf, std::string(c.name)
             + ": a different frame packs to different bytes — the packer reads the planes, not a stale copy");
    }

#ifndef OMT_QA_PLANAR_PREFIX
    /* ---- D: the packer refuses what it cannot honour. These are the refusals that used to be
     * implicit (and silent); here they are named, because a packer that guesses a layout is how
     * this defect came back. ---- */
    {
        FakeImage img(256, 64, 256, 128, false);
        omtpack::Plane y = img.planeY(), u = img.planeU(), v = img.planeV();

        auto refuses = [&](omtpack::Plane yy, omtpack::Plane uu, omtpack::Plane vv,
                           int64_t cap, const char* needle, const std::string& what) {
            std::vector<uint8_t> dst((size_t)std::max<int64_t>(cap, 1 << 20), 0);
            omtpack::Result r{};
            char why[192] = {};
            bool got = omtpack::packNv12(yy, uu, vv, 256, 64, dst.data(), cap, &r, why, sizeof(why));
            bool good = !got && has(why, needle);
            ok(good, what + (good ? std::string() : std::string("   [verdict=") + (got ? "accepted" : "refused")
                              + ", why=\"" + why + "\", wanted \"" + needle + "\"]"));
        };

        omtpack::Plane oddU = u;  oddU.pixelStride = 3;
        omtpack::Plane oddV = v;  oddV.pixelStride = 3;
        refuses(y, oddU, oddV, omtpack::nv12Size(256, 64), "unsupported chroma pixelStride",
                "D1: chroma pixelStride 3 is neither NV12 nor I420 — refused, not guessed");
        refuses(y, u, v, omtpack::nv12Size(256, 64) - 1, "destination buffer smaller",
                "D2: a destination one byte short of stride*height*3/2 is refused before it is written");
        omtpack::Plane tinyU = u; tinyU.size = u.size - 1;
        refuses(y, tinyU, v, omtpack::nv12Size(256, 64), "smaller than (height/2)*rowStride",
                "D3: a chroma plane one byte short is refused rather than read to the last byte");
        omtpack::Plane narrowU = u; narrowU.rowStride = 4;
        refuses(y, narrowU, v, omtpack::nv12Size(256, 64), "rowStride cannot hold one Cb/Cr row",
                "D4: a chroma rowStride narrower than one Cb row is refused (the row would run into the next)");
        omtpack::Plane weirdY = y; weirdY.pixelStride = 2;
        refuses(weirdY, u, v, omtpack::nv12Size(256, 64), "Y plane pixelStride is not 1",
                "D5: an interleave in the luma plane is not something this path can honour");
        omtpack::Plane shortY = y; shortY.size = 100;
        refuses(shortY, u, v, omtpack::nv12Size(256, 64), "Y plane buffer smaller",
                "D6: a Y plane that cannot hold height*rowStride is refused");

        // And the positive control for the refusals above: the same geometry, intact, is accepted.
        std::vector<uint8_t> dst((size_t)omtpack::nv12Size(256, 64), 0);
        omtpack::Result r{};
        char why[192] = {};
        bool got = omtpack::packNv12(y, u, v, 256, 64, dst.data(), (int64_t)dst.size(), &r,
                                     why, (int)sizeof(why));
        ok(got && r.layout == omtpack::Layout::PlanarI420 && r.stride == 256 &&
           r.dataLength == 256 * 64 * 3 / 2,
           "D7: the intact version of that same geometry packs, and names itself planar I420 — "
           "the refusals above are selective, not a packer that always says no");

        // Odd dimensions: chroma is width/2 by height/2, so an odd frame has no honest answer.
        std::vector<uint8_t> dst2(1 << 20, 0);
        omtpack::Result r2{};
        got = omtpack::packNv12(y, u, v, 255, 64, dst2.data(), (int64_t)dst2.size(), &r2, why, sizeof(why));
        ok(!got && has(why, "odd"), "D8: an odd width is refused by the packer too (validateFrame would refuse the frame anyway)");

        ok(omtpack::nv12Size(0, 64) == -1 && omtpack::nv12Size(256, -1) == -1,
           "D9: nv12Size refuses a non-positive geometry instead of returning a size that cannot hold a frame");
        ok(omtpack::nv12Size(1920, 1080) == 3110400,
           "D10: and at the emulator's geometry it asks for exactly the 3110400 bytes the gate demanded "
           "(got " + std::to_string(omtpack::nv12Size(1920, 1080)) + ")");
    }

    /* ---- E: the emulator's symptom, pinned as a number rather than as a memory. ---- */
    {
        FakeImage img(1920, 1080, 1920, 960, false);
        std::vector<uint8_t> legacy = img.legacyCopy();
        int legacyLength = img.legacyDataLength();
        ok(legacy.size() == 2592000 && legacyLength == 2592000,
           "E1: the pre-fix capture path produced a 2592000-byte frame at 1920x1080 stride=1920 — "
           "the exact dataLength in the logcat line this branch exists to erase (got "
           + std::to_string(legacy.size()) + "/" + std::to_string(legacyLength) + ")");
        std::string why;
        MediaFrame f = frameOf(legacy.data(), 1920, 1080, 1920, legacyLength);
        bool valid = Sender::validateFrameForTest(f, why);
        ok(!valid && has(why, "buffer too small for NV12 1920x1080 stride=1920") &&
           has(why, "3110400") && has(why, "2592000"),
           "E2: and the gate still refuses it, quoting both numbers — the fix moves the arithmetic "
           "into the packer, it does not soften the gate" + (valid ? std::string("   [ACCEPTED — the gate changed!]")
            : std::string("   [why=\"") + why + "\"]"));

        Built now = build(img, 0);
        ok(now.packed && now.stride == 1920 && now.dataLength == 3110400,
           "E3: the same camera frame through the packer is 3110400 bytes of real NV12 — what the "
           "encoder asked for, arrived at by writing it rather than by hoping the strides matched");
        expectGoodNv12(now, img, "E4 the emulator's own frame");
    }
#else
    /* The pre-fix binary must say WHY it is red in the operator's words, not just exit non-zero. */
    {
        FakeImage img(1920, 1080, 1920, 960, false);
        std::vector<uint8_t> legacy = img.legacyCopy();
        std::string why;
        MediaFrame f = frameOf(legacy.data(), 1920, 1080, 1920, img.legacyDataLength());
        bool valid = Sender::validateFrameForTest(f, why);
        std::printf("  [info] pre-fix 1920x1080 planar frame: %s   why=\"%s\"\n",
                    valid ? "ACCEPTED BY THE GATE" : "REFUSED BY THE GATE", why.c_str());
        ok(!valid, "prefix: the emulator's frame is refused by the sender's gate — this is the "
                   "logcat line the phone produced on every frame, and the reason the wire carried 0 video frames");
    }
#endif

    close(fd);
    sender.stop();

    std::printf("\nplanar-chroma: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
