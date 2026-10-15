/*
 * wire-declaration.cpp — what the sender tells the receiver must come from the encoder that made
 * the bytes, not from whatever the caller wrote into the struct.
 *
 * The encoder-reuse key (qa/encoder-reconfiguration.cpp) makes the two agree on the live path. This
 * file pins the two things that make that agreement structural rather than a lucky coincidence:
 *
 *   W-A/W-B/W-C — the encoder identity as a key. The recorder in qa/stub_vmx.cpp reports every
 *       VMX_Create() the sender asked for, in order, and stamps each payload with the colour space
 *       of the instance that wrote it. A same-size colour-space change must therefore show up as a
 *       NEW create carrying the new colour space, and the header a subscriber reads must equal the
 *       stamp inside the payload it arrived with. Reusing an encoder across the change is visible
 *       here as createRecords staying at 1 and the stamp disagreeing with the header.
 *
 *   W-D..W-I — the declaration function itself. Sender::buildVideoHeaders() is pure and takes
 *       (frame, encoder identity). Feeding it a contradictory pair — a frame claiming 709 against an
 *       encoder created as 601 — is the only way to watch the derivation, because with the key
 *       correct the live path can never produce that pair. A gate nobody can drive is not evidence,
 *       so the seam exists and qa/build-host.sh mutates each clause of it separately.
 *
 *   W-J/W-K — the numbers and predicates the gates are written against: the sender's VideoFlags must
 *       equal the public OMTVideoFlags enum (the wire numbering was already wrong once, in the
 *       colour block), and the interlace height rule must be the codec's rule
 *       (libvmx/src/vmxcodec.cpp:501-510), not an invention of this file.
 *
 * Build + run:  bash qa/build-host.sh   (which also runs the mutation controls)
 */
#include "omt-wire.h"

#include "OMTProtocol.h"
#include "OMTSender.h"
#include "libomt.h"      // OMTVideoFlags / OMTColorSpace: the authority the wire numbering follows
#include "vmxcodec.h"    // VMX_PROFILE_*: what the recorder reports being asked to create

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace omt;
using omtqa::Tally;
using omtqa::Stream;
using omtqa::Wire;

// The recorder side of qa/stub_vmx.cpp.
namespace qa_stub {
    extern int failWidth, failHeight;
    extern int payloadBytes;
    extern int createCalls, destroyCalls, encodeCalls, saveCalls;
    struct Create { int width, height, profile, colorSpace; };
    extern Create creates[32];
    extern int createRecords;
    extern unsigned kPayloadMagic;
    void reset();
}

static Tally t;

static MediaFrame bgra(void* px, int w, int h, int cs, int64_t ts) {
    MediaFrame f{};
    f.data = px;
    f.dataLength = w * h * 4;
    f.width = w; f.height = h;
    f.stride = w * 4;
    f.codec = Codec::BGRA;
    f.frameRateN = 30; f.frameRateD = 1;
    f.aspectRatio = (float)w / (float)h;
    f.flags = 0;
    f.colorSpace = cs;
    f.timestamp = ts;
    return f;
}

// The recorder's own provenance stamp, read back out of a payload the subscriber received.
struct Provenance {
    bool ok = false;
    int colorSpace = 0, width = 0, height = 0, seq = 0;
};
static Provenance parseProvenance(const std::vector<uint8_t>& p) {
    Provenance pr;
    if (p.size() < 20) return pr;
    unsigned magic = 0;
    std::memcpy(&magic, p.data(), 4);
    if (magic != qa_stub::kPayloadMagic) return pr;
    std::memcpy(&pr.colorSpace, p.data() + 4, 4);
    std::memcpy(&pr.width, p.data() + 8, 4);
    std::memcpy(&pr.height, p.data() + 12, 4);
    std::memcpy(&pr.seq, p.data() + 16, 4);
    pr.ok = true;
    return pr;
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);

    const int W = 64, H = 64;
    std::vector<uint8_t> px((size_t)W * H * 4, 0x3C);
    for (size_t i = 0; i < px.size(); ++i) px[i] = (uint8_t)((i * 31 + (i >> 6)) & 0xFF);

    // ---- W-A: the key. Same size, colour space moving: each hop must be a NEW encoder, created as
    // the colour space its frames are then declared to be, and the payload's own provenance stamp
    // must be the value the subscriber's header claims.
    {
        Stream s(45351, "qa-wire-decl-key", t);
        if (s.reader) {
            qa_stub::reset();
            const int seq[4] = {ColorSpace::BT601, ColorSpace::BT709, ColorSpace::BT709,
                                ColorSpace::BT601};
            for (int i = 0; i < 4; ++i) {
                if (!s.sendDelivered(bgra(px.data(), W, H, seq[i], i + 1), t,
                                     "WA" + std::to_string(i + 1) + ": frame at cs=" +
                                     std::to_string(seq[i]))) continue;
                Wire w = s.reader->next();
                t.ok(w.got && w.ext.colorSpace == seq[i],
                     "WA" + std::to_string(i + 5) + ": header declares " + std::to_string(seq[i]));
                const Provenance pr = parseProvenance(w.payload);
                t.ok(pr.ok && pr.colorSpace == seq[i],
                     "WA" + std::to_string(i + 9) + ": the payload was written by an encoder created as " +
                     std::to_string(seq[i]) + " (stamp says " + std::to_string(pr.colorSpace) +
                     ") — a header may only advertise the matrix that produced these bytes");
                t.ok(pr.ok && pr.width == W && pr.height == H,
                     "WA" + std::to_string(i + 13) + ": and the same goes for the size the header carries");
            }
            // 601, 709, (709 again: reused), 601 — three creates for four frames, and the second and
            // third frames share one encoder. Not four creates: that is the churn S_D forbids.
            t.ok(qa_stub::createRecords == 3,
                 "WA17: exactly three VMX_Create calls for four frames — one per colour-space change, "
                 "none for a frame that changed nothing (recorded " +
                 std::to_string(qa_stub::createRecords) + ")");
            const int want[3] = {ColorSpace::BT601, ColorSpace::BT709, ColorSpace::BT601};
            bool keyOk = qa_stub::createRecords == 3;
            for (int i = 0; keyOk && i < 3; ++i)
                keyOk = qa_stub::creates[i].width == W && qa_stub::creates[i].height == H &&
                        qa_stub::creates[i].colorSpace == want[i];
            t.ok(keyOk, "WA18: and each create names the size and the colour space the frames between "
                        "it and the next create were declared to use");
            const EncoderIdentity live = s.sender.encoderIdentity();
            t.ok(live.valid && live.colorSpace == ColorSpace::BT601 && live.width == W && live.height == H,
                 "WA19: the live encoder identity agrees with the last declaration (" +
                 std::to_string(live.width) + "x" + std::to_string(live.height) + " cs=" +
                 std::to_string(live.colorSpace) + " valid=" + (live.valid ? "1" : "0") + ")");
        }
    }

    // ---- W-B: no churn when nothing changed (the same property encoder-reconfiguration S_D checks
    // through the bitstream, here through the create log).
    {
        Stream s(45352, "qa-wire-decl-nochurn", t);
        if (s.reader) {
            qa_stub::reset();
            for (int i = 1; i <= 4; ++i)
                s.sendDelivered(bgra(px.data(), W, H, ColorSpace::BT709, i), t,
                                "WB" + std::to_string(i) + ": unchanged frame " + std::to_string(i));
            t.ok(qa_stub::createRecords == 1,
                 "WB5: four identical-descriptor frames, one VMX_Create (recorded " +
                 std::to_string(qa_stub::createRecords) + ") — the key reuses when reuse is right");
            t.ok(qa_stub::encodeCalls == 4,
                 "WB6: and every frame really went to that encoder (encodeCalls=" +
                 std::to_string(qa_stub::encodeCalls) + ")");
        }
    }

    // ---- W-C: profile is still part of the encoder's real identity. It is not in the createEncoder
    // key (every path that changes it calls destroyEncoder()), so pin the behaviour, not the key.
    {
        Stream s(45353, "qa-wire-decl-profile", t);
        if (s.reader) {
            qa_stub::reset();
            s.sendDelivered(bgra(px.data(), W, H, ColorSpace::BT709, 1), t, "WC1: first frame");
            s.reader->next();
            s.sender.setQuality(Quality::High);
            s.sendDelivered(bgra(px.data(), W, H, ColorSpace::BT709, 2), t, "WC2: after a quality change");
            s.reader->next();
            t.ok(qa_stub::createRecords == 2 &&
                 qa_stub::creates[1].profile == VMX_PROFILE_OMT_HQ,
                 "WC3: a quality change rebuilt the encoder with the new profile (profiles " +
                 std::to_string(qa_stub::createRecords > 1 ? qa_stub::creates[0].profile : -1) + " -> " +
                 std::to_string(qa_stub::createRecords > 1 ? qa_stub::creates[1].profile : -1) + ")");
        }
    }

    // ---- W-D..W-I: the declaration function on its own, driven with pairs the live path cannot
    // produce. Each clause has its own mutant in qa/build-host.sh.
    {
        MediaFrame f = bgra(px.data(), W, H, ColorSpace::BT709, 7);
        EncoderIdentity enc; enc.width = W; enc.height = H; enc.colorSpace = ColorSpace::BT709;
        enc.profile = VMX_PROFILE_OMT_SQ; enc.valid = true;
        FrameHeader outH{}; VideoExtHeader outE{}; std::string why;

        t.ok(Sender::buildVideoHeadersForTest(f, enc, 1024, outH, outE, why),
             "WD: a frame and an encoder that agree produce headers (" + why + ")");
        t.ok(outE.colorSpace == enc.colorSpace && outE.width == enc.width && outE.height == enc.height &&
             outH.dataLength == VideoExtHeader::SIZE + 1024 &&
             outH.frameType == (uint8_t)FrameType::Video && outH.version == 1,
             "WD2: with the size, type and length derived from the encoder and the payload");

        // The contradiction the whole branch exists for: caller claims 709, encoder was created 601.
        EncoderIdentity lying = enc; lying.colorSpace = ColorSpace::BT601;
        why.clear();
        t.ok(!Sender::buildVideoHeadersForTest(f, lying, 1024, outH, outE, why),
             "WE: a frame claiming 709 over an encoder created as 601 is REFUSED, not published");
        t.ok(Tally::has(why, "709") && Tally::has(why, "601"),
             "WE2: and the reason names both numbers so an operator can see which is which: \"" + why + "\"");

        // ... and the mirror: the encoder is the authority, so a frame claiming 601 over a 709 encoder
        // is refused too rather than quietly corrected on the wire.
        MediaFrame f601 = f; f601.colorSpace = ColorSpace::BT601;
        why.clear();
        t.ok(!Sender::buildVideoHeadersForTest(f601, enc, 1024, outH, outE, why),
             "WE3: the same refusal in the other direction — the sender does not repair a claim, it "
             "declines to make one");

        // Dimensions are declared from the encoder too.
        EncoderIdentity wrongSize = enc; wrongSize.width = 128;   // valid, agrees on colour, wrong size
        why.clear();
        t.ok(!Sender::buildVideoHeadersForTest(f, wrongSize, 1024, outH, outE, why),
             "WF: an encoder of a different size cannot stand behind this frame: \"" + why + "\"");

        // No encoder, no declaration. The identity agrees with the frame in every other respect on
        // purpose: if it disagreed, the size/colour-space clauses would refuse it too and the
        // encoder.valid gate could be deleted without this file noticing (qa/build-host.sh MW5).
        EncoderIdentity none = enc;   none.valid = false;
        why.clear();
        t.ok(!Sender::buildVideoHeadersForTest(f, none, 1024, outH, outE, why),
             "WF2: with no encoder alive there is nothing to declare from: \"" + why + "\"");

        // A matrix the codec does not implement, even coming from the encoder side.
        EncoderIdentity bt2020 = enc; bt2020.colorSpace = 2020;
        MediaFrame f2020 = f; f2020.colorSpace = 2020;
        why.clear();
        t.ok(!Sender::buildVideoHeadersForTest(f2020, bt2020, 1024, outH, outE, why),
             "WG: an encoder reporting 2020 is refused at the header builder as well — the belt does "
             "not assume the validateFrame() brace: \"" + why + "\"");

        // No payload, no frame.
        why.clear();
        t.ok(!Sender::buildVideoHeadersForTest(f, enc, 0, outH, outE, why),
             "WH: a zero-length payload is not a frame, whatever the header would have said: \"" + why + "\"");

        // Undefined(0) is a legitimate non-claim and must survive: VMX resolves it internally by
        // height, and the sender publishing "no matrix claimed" is honest where inventing one would
        // not be.
        MediaFrame fU = f; fU.colorSpace = ColorSpace::Undefined;
        EncoderIdentity encU = enc; encU.colorSpace = ColorSpace::Undefined;
        why.clear();
        t.ok(Sender::buildVideoHeadersForTest(fU, encU, 1024, outH, outE, why) &&
             outE.colorSpace == ColorSpace::Undefined,
             "WI: Undefined(0) is published as Undefined — the sender does not guess a matrix to look "
             "more informative (" + why + ")");
    }

    // ---- W-J: the wire numbering, against the authority. OMTProtocol.h once carried a second,
    // contradictory colour numbering inside the very header that defines the wire struct; its flag
    // block carried HighBitDepth=4, which is PreMultiplied on the wire. A constant that no production
    // path reads is only safe until the day one does — validateFrame() reads them now.
    {
        t.ok(VideoFlags::Interlaced == OMTVideoFlags_Interlaced &&
             VideoFlags::Alpha == OMTVideoFlags_Alpha &&
             VideoFlags::PreMultiplied == OMTVideoFlags_PreMultiplied &&
             VideoFlags::Preview == OMTVideoFlags_Preview &&
             VideoFlags::HighBitDepth == OMTVideoFlags_HighBitDepth,
             "WJ1: the sender's VideoFlags equal the public OMTVideoFlags enum "
             "(HighBitDepth=" + std::to_string(VideoFlags::HighBitDepth) +
             ", PreMultiplied=" + std::to_string(VideoFlags::PreMultiplied) + ")");
        static_assert(VideoFlags::HighBitDepth == OMTVideoFlags_HighBitDepth &&
                      VideoFlags::PreMultiplied == OMTVideoFlags_PreMultiplied &&
                      VideoFlags::Interlaced == OMTVideoFlags_Interlaced &&
                      VideoFlags::Alpha == OMTVideoFlags_Alpha &&
                      VideoFlags::Preview == OMTVideoFlags_Preview,
                      "the sender's flag constants drifted from the public enum — receivers would read "
                      "a different flag than the one the payload supports");
        t.ok(ColorSpace::Undefined == OMTColorSpace_Undefined &&
             ColorSpace::BT601 == OMTColorSpace_BT601 &&
             ColorSpace::BT709 == OMTColorSpace_BT709,
             "WJ2: and the colour numbering still matches too (0/601/709, not the old 0/1/2)");
    }

    // ---- W-K: the interlace predicate is the codec's rule, quoted from
    // libvmx/src/vmxcodec.cpp:501-510, not a number invented here.
    {
        bool yes = true, no = true;
        for (int h : {480, 576, 1080}) yes = yes && Sender::interlacedHeightSupportedForTest(h);
        for (int h : {0, 240, 478, 479, 481, 544, 575, 577, 720, 1079, 1081, 1088, 2160})
            no = no && !Sender::interlacedHeightSupportedForTest(h);
        t.ok(yes, "WK1: 480/576/1080 are interlaceable — the three heights VMX_ConfigureInterlaced accepts");
        t.ok(no, "WK2: everything around them is not — an Interlaced header at any other height would "
                 "describe a field order the payload does not have");
    }

    return t.report("wire-declaration");
}
