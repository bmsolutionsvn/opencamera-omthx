/*
 * declaration-gate.cpp — what the wire is allowed to claim about a payload that does not have it.
 *
 * VideoExtHeader publishes `flags` and `aspectRatio` verbatim from the caller's MediaFrame, and
 * `colorSpace` from the encoder behind the payload. MediaFrame is a plain struct that every caller
 * fills by hand (the C entry point at libomt/cpp/libomt_android.cpp:163-179 copies OMTMediaFrame
 * field by field with no interpretation), so nothing in the type system stops a claim the encoder
 * never implemented. This is the family the desktop's lying-Alpha flag belongs to: a header that a
 * same-codec loopback cannot contradict, because both ends agree on the fiction.
 *
 * Every case below is a REFUSAL at Sender::validateFrame(), which runs before the no-subscribers
 * shortcut — a frame that would have lied on air has to be refused on the bench, with nobody
 * watching. Each refusal must be counted (encodeFailures) and named (lastError), because an
 * uncounted, unnamed refusal is the bug b66ab347 was written against.
 *
 * The counterpart cases (SG*) are the anti-vacuity half: legitimate flag combinations must still be
 * accepted, so a gate that simply rejects everything cannot pass this file. They are checked with no
 * subscriber attached, which is exactly why they assert the no-clients code (1) rather than a byte
 * count: the gate under test runs before that shortcut, so a refusal here would be visible either way.
 *
 * Links qa/stub_vmx.cpp rather than the codec: none of these frames reaches the encoder at all, and
 * a stub that cannot encode is what proves the refusal happened at the boundary.
 *
 * Build + run:  bash qa/build-host.sh   (which also runs the mutation controls)
 */
#include "OMTProtocol.h"
#include "OMTSender.h"
#include "libomt.h"      // OMTVideoFlags / OMTColorSpace: the authority for the numbers the wire carries

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace omt;

static int failures = 0;
static void ok(bool cond, const std::string& what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++failures;
}

static const int FW = 64, FH = 64;

static MediaFrame frameOf(void* data, int bytes, int w, int h, int codec, int stride,
                          int colorSpace, int flags, float aspect) {
    MediaFrame f{};
    f.data = data;
    f.dataLength = bytes;
    f.width = w; f.height = h;
    f.stride = stride;
    f.codec = codec;
    f.frameRateN = 30; f.frameRateD = 1;
    f.aspectRatio = aspect;
    f.flags = flags;
    f.colorSpace = colorSpace;
    f.timestamp = 1;
    return f;
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);

    std::vector<uint8_t> px((size_t)FW * FH * 4, 0x3C);
    std::vector<uint8_t> nv((size_t)FH * FW * 3 / 2, 0x3C);
    std::vector<uint8_t> px480((size_t)FW * 480 * 4, 0x3C);

    SenderConfig c; c.name = "qa-declaration-gate"; c.port = 45347; c.quality = Quality::Default;
    Sender s(c);   // deliberately never started: the gate runs before the no-clients shortcut

    // ---- SE: claims the payload cannot keep -----------------------------------------------
    struct Bad { int flags; int codec; int h; const char* what; };
    const Bad bad[] = {
        {OMTVideoFlags_HighBitDepth,  Codec::BGRA, FH,
         "HighBitDepth(16) on an 8-bit-only input path — P216/PA16 never reach encodeFrame()"},
        {OMTVideoFlags_Alpha,         Codec::NV12, FH,
         "Alpha(2) claimed on NV12, which has no alpha plane to carry"},
        {OMTVideoFlags_Alpha,         Codec::UYVY, FH,
         "Alpha(2) claimed on UYVY, likewise"},
        {OMTVideoFlags_PreMultiplied, Codec::BGRA, FH,
         "PreMultiplied(4) without Alpha — it qualifies a channel that is not there"},
        {64,                          Codec::BGRA, FH,
         "an undefined flag bit (0x40), which means nothing on either side of the link"},
        {OMTVideoFlags_Interlaced,    Codec::BGRA, 544,
         "Interlaced(1) at 544 lines: VMX_ConfigureInterlaced interlaces 480/576/1080 only"},
    };
    int n = 0;
    for (const auto& b : bad) {
        ++n;
        const bool nv12 = (b.codec == Codec::NV12);
        const int h = b.h;
        std::vector<uint8_t> buf = (h == FH)
            ? (nv12 ? nv : px)
            : std::vector<uint8_t>((size_t)h * FW * (nv12 ? 3 / 2 : 4), 0x3C);
        MediaFrame f = frameOf(buf.data(), (int)buf.size(), FW, h, b.codec,
                               nv12 ? FW : FW * 4, ColorSpace::BT709, b.flags, 1.0f);
        const long long before = s.encodeFailures();
        ok(s.send(f) == 0, std::string("SE") + std::to_string(n) + ": refused — " + b.what);
        ok(s.encodeFailures() == before + 1,
           std::string("SE") + std::to_string(n) + "b: and counted — a frame that left the camera and "
           "produced nothing must show up somewhere an operator can see");
        ok(!s.lastError().empty(),
           std::string("SE") + std::to_string(n) + "c: and it says why: \"" + s.lastError() + "\"");
    }
    // The interlace refusal must be the codec's rule, not a guess: the three heights VMX honours are
    // accepted, their neighbours are not.
    {
        const long long before = s.encodeFailures();
        MediaFrame f = frameOf(px480.data(), (int)px480.size(), FW, 480, Codec::BGRA, FW * 4,
                               ColorSpace::BT601, OMTVideoFlags_Interlaced, 1.0f);
        ok(s.send(f) == 1, "SE7: Interlaced at 480 lines is honoured — VMX really interlaces that height");
        ok(s.encodeFailures() == before, "SE7b: and is not counted as a failure");
    }

    // ---- SF: the aspect ratio goes on the wire and a receiver divides by it -----------------
    {
        const char* names[] = {"NaN", "Inf", "-Inf", "0", "-1"};
        const float vals[] = {std::nanf(""), INFINITY, -INFINITY, 0.0f, -1.0f};
        for (int i = 0; i < 5; ++i) {
            MediaFrame f = frameOf(px.data(), (int)px.size(), FW, FH, Codec::BGRA, FW * 4,
                                   ColorSpace::BT709, 0, vals[i]);
            ok(s.send(f) == 0, std::string("SF") + std::to_string(i + 1) + ": aspect ratio " +
                                   names[i] + " is refused");
            ok(!s.lastError().empty(), std::string("SF") + std::to_string(i + 1) +
                                           "b: named: \"" + s.lastError() + "\"");
        }
    }

    // ---- SG: the valid neighbours. A gate that rejects everything fails here, which is the point.
    {
        const long long before = s.encodeFailures();
        ok(s.send(frameOf(px.data(), (int)px.size(), FW, FH, Codec::BGRA, FW * 4,
                          ColorSpace::BT709, OMTVideoFlags_Alpha, 1.0f)) == 1,
           "SG1: Alpha on BGRA is honoured — VMX_EncodeBGRA really fills the codec's alpha plane "
           "(vmxcodec.cpp:1825 sets VMX_IMAGE_BGRA; the BGRX variant is the one that memsets it to 255)");
        ok(s.send(frameOf(px.data(), (int)px.size(), FW, FH, Codec::BGRA, FW * 4,
                          ColorSpace::BT709, OMTVideoFlags_Alpha | OMTVideoFlags_PreMultiplied,
                          1.0f)) == 1,
           "SG2: Alpha|PreMultiplied on BGRA is honoured (libomt.h:125 — PreMultiplied qualifies Alpha, "
           "so the pair is legal and the single flag is not)");
        ok(s.send(frameOf(px.data(), (int)px.size(), FW, FH, Codec::BGRA, FW * 4,
                          ColorSpace::BT709, OMTVideoFlags_Preview, 1.0f)) == 1,
           "SG3: Preview alone is honoured — it describes the stream, not the pixels");
        ok(s.send(frameOf(px480.data(), (int)px480.size(), FW, 480, Codec::BGRA, FW * 4,
                          ColorSpace::BT709,
                          OMTVideoFlags_Interlaced | OMTVideoFlags_Alpha | OMTVideoFlags_Preview,
                          1.0f)) == 1,
           "SG4: a legal combination of the legal bits at an interlaceable height is honoured "
           "(544 would not be, 480 is — see SE6/SE7)");
        ok(s.encodeFailures() == before,
           "SG5: none of the accepted frames was counted as a failure");
    }

    // ---- SH: the Android app's own frame shape must still be admissible. OMTBridge.cpp posts NV12,
    // flags None, aspect width/height, colour space from the height rule; if the new gates ever
    // reached that combination, streaming would stop on device with no host test failing.
    {
        const int W = 640, H = 480;
        std::vector<uint8_t> app((size_t)H * W * 3 / 2, 0x40);
        MediaFrame f = frameOf(app.data(), (int)app.size(), W, H, Codec::NV12, W,
                               ColorSpace::BT601 /* height < 720 -> the bridge picks 601 */,
                               OMTVideoFlags_None, (float)W / (float)H);
        const long long before = s.encodeFailures();
        ok(s.send(f) == 1, "SH1: the exact descriptor OMTBridge.cpp posts today is accepted");
        ok(s.encodeFailures() == before, "SH2: and uncounted, as it should be");
        // The HD half of the bridge's height rule: same NV12 shape, 1920x1080, 709.
        const int HW = 1920, HH = 1080;
        std::vector<uint8_t> hd((size_t)HH * HW * 3 / 2, 0x40);
        ok(s.send(frameOf(hd.data(), (int)hd.size(), HW, HH, Codec::NV12, HW,
                         ColorSpace::BT709, OMTVideoFlags_None, (float)HW / (float)HH)) == 1,
           "SH3: and the HD 709 variant the same rule produces is accepted too");
    }

    std::printf("\ndeclaration-gate: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
