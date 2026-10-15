/*
 * encoder-reconfiguration.cpp — the encoder that made the pixels must be the encoder the header
 * describes.
 *
 * THE DEFECT (qa/ANDROID-FORMAT-PLAN-ASTRA-20261015.md, "encoder reuse ignores colour-space changes",
 * parent-verified against libomt/cpp/OMTSender.cpp:315-317): createEncoder() reused a live VMX
 * instance on a key of width+height alone, while the colour space it was handed goes into
 * VMX_Create() (line 324) and the frame's colour space went straight into VideoExtHeader (line 633).
 * VMX selects its RGB<->YUV conversion table from the create-time colour space
 * (vmxcodec.cpp:1448/1466/1654/1673 — `if (instance->ColorSpace == VMX_COLORSPACE_BT601) colorTable
 * = RGB_YUV_601;`), so a same-size 601->709 transition kept encoding BGRA with the 601 matrix while
 * telling every receiver the picture is 709. Same failure family as the desktop's lying Alpha flag:
 * declaration and payload disagreeing, and a same-codec loopback structurally unable to notice
 * because both ends share the mistake.
 *
 * HOW THIS PROVES IT WITHOUT A FAKE ENCODER. The scenario links the REAL codec and lets the payload
 * bytes be the witness. Two measured facts (recorded in qa/COLOR-HONESTY-20261015.md):
 *   1. For a colour-sensitive fixture (saturated RGB bars) the bitstream differs between a 601- and
 *      a 709-created encoder — 665 vs 621 bytes at 64x64. A grey fixture does NOT differ, which is
 *      why the fixture is bars and why R0 asserts the difference before anything else counts.
 *   2. VMX has per-frame rate control, so the Nth encode on an instance depends on its history;
 *      given IDENTICAL history the Nth payload is byte-reproducible across instances. "Which encoder
 *      produced this payload" is therefore decidable by comparing against independently created
 *      reference encoders, built in this process by calling the codec directly — the sender never
 *      sees them, so they cannot agree with it by construction.
 *
 * The discriminating assertions are SA6/SA7 and their mirrors: the frame sent after a same-size
 * colour-space change must carry the payload of an encoder created as the space its header declares,
 * and must NOT carry the continued payload of the previous matrix. On the unmodified tree both fail,
 * on the payload, with the header assertions alongside them passing — i.e. it fails because the
 * stream lies, not because the scenario could not be set up.
 *
 * S_D is the counterpart that keeps the fix honest: frames whose encoder-affecting descriptor does
 * NOT change must keep coming from the SAME encoder (their payload equals the continued reference).
 * A "fix" that recreated the encoder on every frame would pass SA6/SA7 and fail S_D. S_G/S_H are the
 * other direction: valid frames, including the app's own NV12 path, must still be accepted.
 *
 * The boundary refusals of the same family (flags the payload cannot carry, an aspect ratio nobody
 * can render) are in qa/declaration-gate.cpp: they never reach the encoder, so linking the real
 * codec there would only make the mutation matrix slower without making it truer.
 *
 * Build + run:  bash qa/build-host.sh   (which also runs the mutation controls)
 */
#include "omt-wire.h"

#include "OMTSender.h"
#include "OMTProtocol.h"
#include "libomt.h"      // the public C enum: the authority for the flag numbers the wire carries
#include "vmxcodec.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace omt;
using omtqa::Tally;
using omtqa::Stream;
using omtqa::Wire;

static Tally t;

// ---------------------------------------------------------------- the fixture
static const int FW = 64, FH = 64;

// Saturated RGB bars, colour-sensitive on purpose: the 601 and 709 matrices disagree loudly on pure
// red/green/blue, so a payload made under one cannot be mistaken for a payload made under the other.
// A grey fixture is blind to the entire defect (measured: byte-identical bitstreams) — that is the
// trap this fixture exists to avoid, and R0b is the assertion that keeps it from silently returning.
static std::vector<uint8_t> bars() {
    static const uint8_t cols[8][3] = {{255,255,255},{255,255,0},{0,255,255},{0,255,0},
                                       {255,0,255},{255,0,0},{0,0,255},{0,0,0}};
    std::vector<uint8_t> px((size_t)FW * FH * 4);
    for (int y = 0; y < FH; ++y)
        for (int x = 0; x < FW; ++x) {
            int bar = x / 8;
            uint8_t* p = &px[((size_t)y * FW + x) * 4];
            p[0] = cols[bar][2]; p[1] = cols[bar][1]; p[2] = cols[bar][0]; p[3] = 255;  // BGRA
        }
    return px;
}

static MediaFrame frameOf(void* data, int bytes, int w, int h, int codec, int stride,
                          int colorSpace, int flags = 0, float aspect = 1.0f, int64_t ts = 1) {
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
    f.timestamp = ts;
    return f;
}

// ------------------------------------------------- the independent references
// What an encoder created as `cs`, handed `repeats` identical frames, really puts on the wire. Built
// straight from libvmx: these instances never pass through Sender, so an agreement between a
// reference and the sender's payload is evidence rather than a shared assumption.
// The profile matches SenderConfig{Quality::Default} -> selectProfile() -> VMX_PROFILE_OMT_SQ.
static std::vector<uint8_t> referencePayload(int cs, int repeats, const std::vector<uint8_t>& px,
                                             int w, int h) {
    VMX_SIZE sz = {w, h};
    VMX_INSTANCE* inst = VMX_Create(sz, VMX_PROFILE_OMT_SQ, static_cast<VMX_COLORSPACE>(cs));
    if (!inst) return {};
    std::vector<uint8_t> out;
    for (int i = 0; i < repeats; ++i) {
        if (VMX_EncodeBGRA(inst, (BYTE*)px.data(), w * 4, 0) != VMX_ERR_OK) { VMX_Destroy(inst); return {}; }
        std::vector<uint8_t> buf(1u << 20);
        int n = VMX_SaveTo(inst, buf.data(), (int)buf.size());
        out.assign(buf.begin(), n > 0 ? buf.begin() + n : buf.begin());
    }
    VMX_Destroy(inst);
    return out;
}
static bool sameBytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

// ---------------------------------------------------------------------- main
int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    auto px = bars();
    const int bytes = (int)px.size();

    // ---- R0: calibrate the witness. If the two matrices produced the same bits, every assertion
    // below would pass with the fix absent and this file would be worth nothing.
    {
        auto r601_1 = referencePayload(ColorSpace::BT601, 1, px, FW, FH);
        auto r601_2 = referencePayload(ColorSpace::BT601, 2, px, FW, FH);
        auto r709_1 = referencePayload(ColorSpace::BT709, 1, px, FW, FH);
        auto r709_2 = referencePayload(ColorSpace::BT709, 2, px, FW, FH);
        t.ok(!r601_1.empty() && !r709_1.empty(), "R0a: the codec produced reference payloads at all");
        t.ok(!sameBytes(r601_1, r709_1) && !sameBytes(r601_2, r709_2),
             "R0b: ANTI-VACUITY — the fixture is colour-sensitive: a 601 payload and a 709 payload of "
             "the same pixels are different bytes (601x1=" + std::to_string(r601_1.size()) +
             " 709x1=" + std::to_string(r709_1.size()) + "). If this ever goes equal, nothing below "
             "proves anything and the fixture must be made louder.");
        t.ok(sameBytes(r601_1, referencePayload(ColorSpace::BT601, 1, px, FW, FH)) &&
             sameBytes(r709_2, referencePayload(ColorSpace::BT709, 2, px, FW, FH)),
             "R0c: the references are reproducible (equal history => equal bits), so payload equality "
             "is a witness and not the codec's rate control being noisy");
    }

    // ---- S_A: same size, 601 -> 709. The defect, in the direction the Android bridge reaches.
    {
        Stream s(45341, "qa-reconf-601to709", t);
        if (s.reader) {
            const auto r601_1 = referencePayload(ColorSpace::BT601, 1, px, FW, FH);
            const auto r601_2 = referencePayload(ColorSpace::BT601, 2, px, FW, FH);
            const auto r709_1 = referencePayload(ColorSpace::BT709, 1, px, FW, FH);

            if (s.sendDelivered(frameOf(px.data(), bytes, FW, FH, Codec::BGRA, FW * 4,
                                        ColorSpace::BT601, 0, 1.0f, 1), t, "SA1: first frame (601)")) {
                Wire w1 = s.reader->next();
                t.ok(w1.got && w1.ext.colorSpace == ColorSpace::BT601, "SA2: its header declares 601");
                t.ok(w1.got && sameBytes(w1.payload, r601_1),
                     "SA3: baseline — a first frame from a fresh sender carries the 601 reference payload, "
                     "so the reference model itself is right (" + std::to_string(w1.payload.size()) +
                     " vs " + std::to_string(r601_1.size()) + " bytes)");
            }

            if (s.sendDelivered(frameOf(px.data(), bytes, FW, FH, Codec::BGRA, FW * 4,
                                        ColorSpace::BT709, 0, 1.0f, 2), t,
                                "SA4: same-size 709 frame")) {
                Wire w2 = s.reader->next();
                t.ok(w2.got && w2.ext.colorSpace == ColorSpace::BT709,
                     "SA5: its header declares 709 — that is the claim under test, never the thing "
                     "being proved");
                t.ok(w2.got && sameBytes(w2.payload, r709_1),
                     "SA6: THE GATE — the payload it carries is the one an encoder created as 709 makes "
                     "(got " + std::to_string(w2.payload.size()) + " bytes, the 709 reference is " +
                     std::to_string(r709_1.size()) + "). A same-size colour-space change must not keep "
                     "encoding on the previous matrix");
                t.ok(w2.got && !sameBytes(w2.payload, r601_2),
                     "SA7: and specifically is NOT the continued-601-encoder payload (" +
                     std::to_string(r601_2.size()) + " bytes). Equal bytes here means the encoder was "
                     "reused across the colour-space change while the header advertised the new one.");
            }
        }
    }

    // ---- S_B: the mirror, 709 -> 601. A fix that only recreates in one direction dies here.
    {
        Stream s(45342, "qa-reconf-709to601", t);
        if (s.reader) {
            const auto r709_1 = referencePayload(ColorSpace::BT709, 1, px, FW, FH);
            const auto r601_1 = referencePayload(ColorSpace::BT601, 1, px, FW, FH);
            const auto r709_2 = referencePayload(ColorSpace::BT709, 2, px, FW, FH);
            s.sendDelivered(frameOf(px.data(), bytes, FW, FH, Codec::BGRA, FW * 4,
                                    ColorSpace::BT709, 0, 1.0f, 1), t, "SB1: 709 frame");
            s.reader->next();
            if (s.sendDelivered(frameOf(px.data(), bytes, FW, FH, Codec::BGRA, FW * 4,
                                        ColorSpace::BT601, 0, 1.0f, 2), t, "SB2: same-size 601 frame")) {
                Wire w = s.reader->next();
                t.ok(w.got && w.ext.colorSpace == ColorSpace::BT601, "SB3: header declares 601");
                t.ok(w.got && sameBytes(w.payload, r601_1),
                     "SB4: payload is the 601 reference (" + std::to_string(w.payload.size()) + " vs " +
                     std::to_string(r601_1.size()) + ")");
                t.ok(w.got && !sameBytes(w.payload, r709_2),
                     "SB5: and not the continued-709-encoder payload (" +
                     std::to_string(r709_2.size()) + " bytes)");
            }
        }
    }

    // ---- S_C: 601 -> 709 -> 601 at one size. A sticky encoder cannot survive three hops.
    {
        Stream s(45343, "qa-reconf-roundtrip", t);
        if (s.reader) {
            const int cs_seq[3] = {ColorSpace::BT601, ColorSpace::BT709, ColorSpace::BT601};
            for (int i = 0; i < 3; ++i) {
                const int cs = cs_seq[i];
                if (s.sendDelivered(frameOf(px.data(), bytes, FW, FH, Codec::BGRA, FW * 4, cs, 0,
                                            1.0f, i + 1), t,
                                    "SC" + std::to_string(i + 1) + ": hop " + std::to_string(i + 1) +
                                    " (cs=" + std::to_string(cs) + ")")) {
                    Wire w = s.reader->next();
                    const auto ref = referencePayload(cs, 1, px, FW, FH);
                    t.ok(w.got && w.ext.colorSpace == cs,
                         "SC" + std::to_string(i + 4) + ": header declares " + std::to_string(cs));
                    t.ok(w.got && sameBytes(w.payload, ref),
                         "SC" + std::to_string(i + 7) + ": payload is the " + std::to_string(cs) +
                         " reference, not a previous hop's matrix (" + std::to_string(w.payload.size()) +
                         " vs " + std::to_string(ref.size()) + ")");
                }
            }
        }
    }

    // ---- S_D: the counterpart gate. Nothing encoder-affecting changed => the SAME encoder keeps
    // producing, i.e. the payload is the CONTINUED reference. Recreating every frame would make
    // these bits wrong (and would torch the camera thread for no reason).
    {
        Stream s(45344, "qa-reconf-no-churn", t);
        if (s.reader) {
            for (int i = 1; i <= 3; ++i) {
                if (s.sendDelivered(frameOf(px.data(), bytes, FW, FH, Codec::BGRA, FW * 4,
                                            ColorSpace::BT709, 0, 1.0f, i), t,
                                    "SD" + std::to_string(i) + ": unchanged frame " + std::to_string(i))) {
                    Wire w = s.reader->next();
                    const auto cont = referencePayload(ColorSpace::BT709, i, px, FW, FH);
                    t.ok(w.got && sameBytes(w.payload, cont),
                         "SD" + std::to_string(i + 3) + ": frame " + std::to_string(i) + " carries the "
                         "continued-encoder payload (" + std::to_string(w.payload.size()) + " vs " +
                         std::to_string(cont.size()) + ") — no gratuitous re-create when nothing changed");
                }
            }
        }
    }

    // ---- S_H: the app's own path. NV12 at a fixed size across a colour-space change must keep
    // flowing, not be refused by the new key. (OMTBridge.cpp sends NV12 in a height-derived space.)
    {
        Stream s(45346, "qa-reconf-nv12", t);
        if (s.reader) {
            const int W = 640, H = 480;
            std::vector<uint8_t> nv((size_t)H * W * 3 / 2, 0x40);
            for (size_t i = 0; i < nv.size(); ++i) nv[i] = (uint8_t)(i * 7 + (i >> 8));
            for (int i = 0; i < 2; ++i) {
                const int cs = i ? ColorSpace::BT709 : ColorSpace::BT601;
                MediaFrame f = frameOf(nv.data(), (int)nv.size(), W, H, Codec::NV12, W, cs,
                                       0, (float)W / H, i + 1);
                if (s.sendDelivered(f, t, "SH" + std::to_string(i + 1) + ": NV12 " + std::to_string(W) +
                                        "x" + std::to_string(H) + " at cs=" + std::to_string(cs))) {
                    Wire w = s.reader->next();
                    t.ok(w.got && w.ext.colorSpace == cs && w.ext.width == W && w.ext.height == H,
                         "SH" + std::to_string(i + 3) + ": and its header describes it");
                }
            }
        }
    }

    return t.report("encoder-reconfiguration");
}
