/*
 * planar-chroma.cpp — the capture path must honour whichever chroma layout the HAL reported.
 *
 * ImageFormat.YUV_420_888 does not name a memory layout. It names three planes, and the HAL fills in
 * the description; planes[1].getPixelStride() is one input to which chroma you got, and — as of the
 * defect this file now measures — NOT a sufficient one. The contract
 * (https://developer.android.com/reference/android/graphics/ImageFormat#YUV_420_888) guarantees only
 * the plane ORDER (Y, Cb, Cr), a luma pixelStride of 1, and that the U and V strides are equal. It
 * never says a pixelStride==2 U buffer holds a complete contiguous NV12 chroma plane, and the shapes
 * below include three where that is false.
 *
 * The capture path assumed it was. It copied planes[1] verbatim behind the luma plane, then charged
 * the frame for height*yStride + (height/2)*uvStride (app/src/main/cpp/OMTBridge.cpp:121 before this
 * fix) while the encoder walks BOTH planes at the single declared stride (OMTSender.cpp:374-377 —
 * uv = data + stride*height; VMX_EncodeNV12(y, stride, uv, stride)). On a planar device uv rowStride
 * is width/2, so the frame came up half a chroma plane short and Sender::validateFrame() — correctly
 * — refused every single one:
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
 *                gate unchanged, (b) really encodes to a subscriber, and (c) equals, byte for byte,
 *                the frame qa/nv12-oracle.h builds from the plane descriptors alone. That last
 *                clause is the whole point of the rewrite: the assertion never asks the packer what
 *                it decided, so a packer that guesses a layout cannot grade its own homework.
 *   pc-prefix  — the same scenarios against a verbatim transcription of the pre-fix capture path
 *                (Java copy + OMTBridge.cpp:121 arithmetic). Case B is the emulator's geometry and
 *                MUST go red there, with the 3110400-vs-2592000 refusal named in the output. If this
 *                binary ever comes back green, the harness has stopped measuring the defect.
 *   mutants    — omtnv12.h with one property sabotaged each. mp1..mp5 are the original five (ignore
 *                pixelStride, forget to interleave, walk the chroma row with the luma stride, charge
 *                the old dataLength, park the UV plane at the wrong offset). mq1..mq3 are the new
 *                gates: copy the U buffer without proving its window is the plane, read a VU-backed
 *                pair in UV order, and bound only the U window. Each must die, and each dies for its
 *                own reason: mp1 and mp3 to ASan (a plane read past its end), mq2 to the byte oracle
 *                (a colour swap is content), mq3 to the one named refusal it was supposed to make.
 *                ASan only sees the first two, which is the whole reason qa/fake-image.h allocates
 *                every plane to its exact reported limit and not one byte more.
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

#include "fake-image.h"
#include "nv12-oracle.h"

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
using fakehal::Shape;
using FakeImage = fakehal::FakeImage;

static int failures = 0;

static void ok(bool cond, const std::string& what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++failures;
}
static bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

static omtpack::Plane toPlane(const oracle::RawPlane& p) {
    return omtpack::Plane{p.data, p.size, p.rowStride, p.pixelStride};
}

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
    omtpack::Layout layout = omtpack::Layout::Unknown;
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
    omtpack::Plane y = toPlane(img.planeY()), u = toPlane(img.planeU()), v = toPlane(img.planeV());
    b.packed = omtpack::packNv12(y, u, v, img.w, img.h, b.buf.data(), need + dstSlop, &r,
                                why, (int)sizeof(why));
    b.stride = r.stride;
    b.dataLength = (int)r.dataLength;
    b.layout = r.layout;
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

    /* THE ORACLE. qa/nv12-oracle.h builds the expected frame from the plane descriptors alone —
     * sample(r,c) = plane.buffer[row*rowStride + col*pixelStride], per component — which is Android's
     * definition of a plane and contains no notion of "layouts" at all. Every byte of what the packer
     * wrote is compared against it: luma rows, both chroma components interleaved, and the padding
     * the packer owes the encoder. Nothing here asks the packer what it decided and then checks that
     * it decided that. */
    oracle::Expected e = oracle::nv12(img.planeY(), img.planeU(), img.planeV(), img.w, img.h);
    if (e.harnessError) {
        ok(false, tag + ": FIXTURE ERROR — " + e.why);
        return;
    }
    long long badAt = -1;
    const size_t n = std::min(b.buf.size(), e.bytes.size());
#ifdef OMT_QA_PLANAR_PREFIX
    /* The pre-fix path forwarded the HAL's own padding bytes instead of zeroing them, and scoring
     * that here would make pc-prefix red on the interleaved devices it actually served correctly —
     * a harness that is red everywhere is not measuring the planar defect any more. The luma and
     * chroma VISIBLE regions are still compared in full. */
    auto isPaddingColumn = [&](size_t i) { return (i % (size_t)b.stride) >= (size_t)img.w; };
    for (size_t i = 0; i < n; ++i) if (!isPaddingColumn(i) && b.buf[i] != e.bytes[i]) { badAt = (long long)i; break; }
#else
    for (size_t i = 0; i < n; ++i) if (b.buf[i] != e.bytes[i]) { badAt = (long long)i; break; }
#endif
    ok(badAt < 0 && b.buf.size() == e.bytes.size(),
       tag + ": every one of the " + std::to_string(e.bytes.size()) + " packed bytes equals the oracle"
             + (badAt < 0 ? std::string()
                          : std::string("   [first mismatch at byte ") + std::to_string(badAt) + ": got 0x"
                          + [&]{ char t[8]; snprintf(t,sizeof t,"%02X",b.buf[(size_t)badAt]); return std::string(t); }()
                          + " want 0x"
                          + [&]{ char t[8]; snprintf(t,sizeof t,"%02X",e.bytes[(size_t)badAt]); return std::string(t); }()
                          + " — the plane was copied, walked with the wrong stride, swapped, or not interleaved]")
       );

#ifndef OMT_QA_PLANAR_PREFIX
    // A property of the packer, not of the defect: the pre-fix path forwarded the HAL's padding
    // bytes as they were. Asserting it in both builds would make pc-prefix red for reasons that
    // are not the bug, and a harness that is red everywhere proves nothing.
    bool padOk = true;
    for (int r = 0; r < img.h && padOk; ++r)
        for (int c = img.w; c < b.stride; ++c)
            if (b.buf[(size_t)r * b.stride + c] != 0) { padOk = false; break; }
    ok(padOk, tag + ": luma padding past the visible row is zeroed, not whatever the HAL left there");

    const uint8_t* uv = b.buf.data() + (size_t)b.stride * img.h;
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
        Shape shape;
        int64_t uTrim;       // bytes shaved off the Cb plane's reported limit
        bool    chromaPad;   // the shared chroma allocation carries one byte past the last pair
        const char* wantLayoutName;  // a substring of layoutName(), not an enumerator: this file is
                                     // also built against the PRE-FIX header, whose enum has no name
                                     // for shapes it could not tell apart, and a scenario that will
                                     // not compile is not a red test — it is no test at all.
        bool newShape = false;       // the shapes pixelStride==2 used to be guessed about; see the
                                     // OMT_QA_PLANAR_PREFIX guard in the loop below for why the
                                     // pre-fix binary does not run them
    };
    const Case cases[] = {
        // (a) the layout the code assumed, and the one real phones with NV12 HALs still report.
        //     yStride == uvStride: this is the shape that must keep working exactly as it did.
        //     planes[2] is planes[1] offset by ONE BYTE inside ONE allocation, which is what a
        //     CB_CR device reports — the earlier version of this fixture gave the two planes
        //     separate allocations with identical contents, a shape Android does not produce, and
        //     the only thing that ever made "copy the U buffer" look correct.
        {"A interleaved NV12, no padding",       256,  64, 256, 256, Shape::SemiPlanarUV, 0, false,
         "CB_CR"},
        {"A2 interleaved NV12, padded luma",     256,  64, 320, 320, Shape::SemiPlanarUV, 0, false,
         "CB_CR"},
        // (b) THE EMULATOR. 1920x1080, y rowStride 1920, chroma rowStride 960 = width/2, chroma
        //     pixelStride 1 — measured off emulator-5554, and the geometry that produced
        //     "the encoder reads 3110400 bytes, dataLength is 2592000" on every frame.
        {"B planar I420, emulator geometry",     1920, 1080, 1920, 960, Shape::Planar, 0, false,
         "I420"},
        {"B2 planar I420, small",                256,  64, 256, 128, Shape::Planar, 0, false,
         "I420"},
        {"B3 planar I420, padded luma",          256,  64, 320, 160, Shape::Planar, 0, false,
         "I420"},
        // (c) the pathological stride neighbourhood the gate exists for, in both directions. C1 is
        //     the case qa/frame-validation.cpp A21 refuses; C2 has a chroma row WIDER than the
        //     luma row, which the old code copied whole and the encoder then walked at yStride —
        //     the picture comes out sheared, and only a content assertion can see it.
        {"C1 interleaved, uv rowStride < y rowStride", 256, 64, 320, 256, Shape::SemiPlanarUV, 0, false,
         "CB_CR"},
        {"C2 interleaved, uv rowStride > y rowStride", 256, 64, 256, 384, Shape::SemiPlanarUV, 0, false,
         "CB_CR"},
        {"C3 planar, chroma row wider than luma row",  256, 64, 256, 200, Shape::Planar, 0, false,
         "I420"},
        // (d) the shapes pixelStride==2 does NOT describe, and which the packer used to read as if it
        //     did. V1 is the CB_CR/CR_CB choice the HAL makes: one allocation, Cb at the ODD byte, so
        //     the U window is not an NV12 plane and copying it shears every chroma row. V2 is the same
        //     frame with a padded luma stride; V3 is the same frame again, but with the Cb view
        //     extended over the whole row — the variant the old code accepted, and the one whose bytes
        //     were wrong rather than merely refused.
        {"V1 VU-backed shared chroma (Cr first)",       256, 64, 256, 256, Shape::SemiPlanarVU, 0, false,
         "CR_CB", true},
        {"V2 VU-backed shared chroma, padded luma",     256, 64, 320, 320, Shape::SemiPlanarVU, 0, false,
         "CR_CB", true},
        {"V3 VU-backed shared chroma, full-row Cb view",256, 64, 256, 256, Shape::SemiPlanarVU, 0, true,
         "CR_CB", true},
        // (e) two allocations that each carry ONE component at stride-2 spacing. The odd byte of the
        //     Cb buffer is the HAL's padding, and forwarding it as chroma is defect (1) in colour.
        {"S1 separate stride-2 Cb and Cr allocations",  256, 64, 256, 256, Shape::SeparateStride2, 0, false,
         "separate stride-2", true},
        {"S2 separate stride-2, padded luma",            256, 64, 320, 320, Shape::SeparateStride2, 0, false,
         "separate stride-2", true},
        // (f) Image.Plane.getBuffer() only guarantees data up to the last sample of the last row, so a
        //     Cb view may stop one byte before the end of the pair it belongs to. Requiring the whole
        //     row there rejects a valid frame — which is how an over-strict "robustness" check takes a
        //     working phone off the wire.
        {"T1 interleaved NV12, Cb view ending at its last Cb sample", 256, 64, 256, 256,
         Shape::SemiPlanarUV, 1, false, "CB_CR", true},
        {"T2 planar I420, Cb view trimmed to its last sample",        256, 64, 320, 160,
         Shape::Planar, 160 - 128, false, "I420", true},
    };

    for (const auto& c : cases) {
#ifdef OMT_QA_PLANAR_PREFIX
        /* The pre-fix binary scores the scenarios it was written to score — the planar frame the
         * emulator actually reported — and its red set has to stay legible as that one defect. The
         * shapes below that line are the second half of the story, and the pre-fix capture path does
         * not merely mislabel them: on V1 its legacyCopy() hands the encoder a buffer one byte short
         * of the dataLength it declares, which dies inside VMX_NV12ToPlanar before the rest of the
         * suite can name anything. That death is measured where it belongs — by the mq mutants, and
         * by building this same file against the pre-fix header — not here, where it would replace
         * the 3110400-vs-2592000 refusal with an unrelated abort. */
        if (c.newShape) continue;
#endif
        FakeImage img(c.w, c.h, c.yRow, c.chromaRow, c.shape, c.chromaPad);
        img.uTrim = c.uTrim;
        std::printf("  -- %s: %dx%d y rowStride=%d, chroma rowStride=%d pixelStride=%d%s\n",
                    c.name, c.w, c.h, c.yRow, c.chromaRow,
                    c.shape == Shape::Planar ? 1 : 2, c.uTrim ? " (Cb view trimmed by 1)" : "");
        Built b = build(img, 0);
        expectGoodNv12(b, img, c.name);
#ifndef OMT_QA_PLANAR_PREFIX
        // The name in the log is the operator's only evidence of what the HAL reported, so the shape
        // the fixture built has to be the shape the packer says it decoded.
        ok(b.packed && has(omtpack::layoutName(b.layout), c.wantLayoutName), std::string(c.name)
             + ": the packer names the layout the descriptors describe, in the words the operator "
               "reads in logcat (" + omtpack::layoutName(b.layout) + ")"
             + (b.packed && has(omtpack::layoutName(b.layout), c.wantLayoutName)
                ? std::string() : std::string("   [the name must contain \"") + c.wantLayoutName + "\"]"));
#endif

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
        FakeImage moved(c.w, c.h, c.yRow, c.chromaRow, c.shape, c.chromaPad);
        moved.uTrim = c.uTrim;
        moved.shiftLuma();
        Built b2 = build(moved, 0);
        ok(b2.buf != b.buf, std::string(c.name)
             + ": a different frame packs to different bytes — the packer reads the planes, not a stale copy");
    }

#ifndef OMT_QA_PLANAR_PREFIX
    /* ---- D: the packer refuses what it cannot honour. These are the refusals that used to be
     * implicit (and silent); here they are named, because a packer that guesses a layout is how this
     * defect came back. ---- */
    {
        FakeImage img(256, 64, 256, 128, Shape::Planar);
        omtpack::Plane y = toPlane(img.planeY()), u = toPlane(img.planeU()), v = toPlane(img.planeV());

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

        /* D7/D8: the two halves of the claim that used to be made from pixelStride alone. The Cr
         * window is now bounded on its own (D7), and a pair of stride-2 planes that share bytes
         * without forming a Cb/Cr pair is refused rather than guessed at (D8). */
        omtpack::Plane tinyV = v; tinyV.size = v.size - 1;
        refuses(y, u, tinyV, omtpack::nv12Size(256, 64), "Cr chroma buffer smaller than (height/2)*rowStride",
                "D7: a Cr plane one byte short is refused on ITS OWN window — the U plane's size is not "
                "evidence about the V buffer, and reading the last Cr anyway is a read past the plane");
        omtpack::Plane mismatchV = v; mismatchV.pixelStride = 2;
        refuses(y, u, mismatchV, omtpack::nv12Size(256, 64), "chroma pixelStrides differ",
                "D8: Cb at pixelStride 1 with Cr at 2 is not a layout Camera2 can produce — refused, not averaged");

        FakeImage semi(256, 64, 256, 256, Shape::SemiPlanarUV);
        omtpack::Plane su = toPlane(semi.planeU()), sv = toPlane(semi.planeV());
        omtpack::Plane sameBaseV = sv; sameBaseV.data = su.data;      // both components claim byte 0
        refuses(y, su, sameBaseV, omtpack::nv12Size(256, 64), "same base address",
                "D9: Cb and Cr at one base with pixelStride 2 asserts Cr==Cb for every sample — that is a "
                "broken description, and folding the colour channels together is not an honourable read");
        omtpack::Plane shearV = sv; shearV.data = su.data + 2; shearV.size = su.size - 2;
        refuses(y, su, shearV, omtpack::nv12Size(256, 64), "not a Cb/Cr pair",
                "D10: two stride-2 windows that overlap at +2 are refused instead of being read as whichever "
                "ordering would make the check pass");

        // And the positive control for the refusals above: the same geometry, intact, is accepted.
        std::vector<uint8_t> dst((size_t)omtpack::nv12Size(256, 64), 0);
        omtpack::Result r{};
        char why[192] = {};
        bool got = omtpack::packNv12(y, u, v, 256, 64, dst.data(), (int64_t)dst.size(), &r,
                                     why, (int)sizeof(why));
        ok(got && r.layout == omtpack::Layout::PlanarI420 && r.stride == 256 &&
           r.dataLength == 256 * 64 * 3 / 2,
           "D11: the intact version of that same geometry packs, and names itself planar I420 — "
           "the refusals above are selective, not a packer that always says no");

        // Odd dimensions: chroma is width/2 by height/2, so an odd frame has no honest answer.
        std::vector<uint8_t> dst2(1 << 20, 0);
        omtpack::Result r2{};
        got = omtpack::packNv12(y, u, v, 255, 64, dst2.data(), (int64_t)dst2.size(), &r2, why, sizeof(why));
        ok(!got && has(why, "odd"), "D12: an odd width is refused by the packer too (validateFrame would refuse the frame anyway)");

        ok(omtpack::nv12Size(0, 64) == -1 && omtpack::nv12Size(256, -1) == -1,
           "D13: nv12Size refuses a non-positive geometry instead of returning a size that cannot hold a frame");
        ok(omtpack::nv12Size(1920, 1080) == 3110400,
           "D14: and at the emulator's geometry it asks for exactly the 3110400 bytes the gate demanded "
           "(got " + std::to_string(omtpack::nv12Size(1920, 1080)) + ")");
    }

    /* ---- E: the emulator's symptom, pinned as a number rather than as a memory. ---- */
    {
        FakeImage img(1920, 1080, 1920, 960, Shape::Planar);
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
        FakeImage img(1920, 1080, 1920, 960, Shape::Planar);
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
