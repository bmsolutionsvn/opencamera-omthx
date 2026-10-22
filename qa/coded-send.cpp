/*
 * coded-send.cpp — an already-encoded frame (OMT-HX) must pass through the sender untouched.
 *
 * WHAT IS UNDER TEST
 * libomt/cpp/OMTSender.cpp used to accept only raw pictures and encode them to VMX. OMT-HX is the
 * same OMT stream carrying an HEVC/H.264 access unit instead: the payload is already coded, the
 * header's codec FourCC says which codec made it, and the sender has nothing to do but pass it on.
 * This file drives the REAL Sender over a REAL socket (qa/omt-wire.h), so what it asserts is what a
 * receiver reads off the wire, not what the sender believes it wrote.
 *
 * The three things that are easy to get wrong, and each has a mutant in qa/build-host.sh:
 *   - the codec tag (a payload tagged VMX1 is a black picture on the receiver);
 *   - the geometry (an Hx source is compared against the decoded picture: a stale size is a silent
 *     decode error);
 *   - the timestamp (100 ns ticks; the receiver measures frame periods from it).
 *
 * Build + run:  bash qa/build-host.sh   (which also runs the mutation controls)
 */
#include "omt-wire.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace omt;

static omtqa::Tally g_tally;
static void ok(bool cond, const std::string& what) {
    // Count into the Tally, not a local: report() is what turns a failed assertion into exit code 1,
    // and a local counter would leave the harness scoring this scenario green whatever it printed.
    g_tally.ok(cond, what);
}

// The frame a MediaCodec-backed caller posts: width/height/fps/colour space per frame, one access
// unit as the payload, stride irrelevant (set to width so a raw-path read would still be in bounds).
static MediaFrame codedFrame(const std::vector<uint8_t>& data, int codec, int w, int h, int64_t ts) {
    MediaFrame f{};
    f.data = const_cast<uint8_t*>(data.data());
    f.dataLength = (int)data.size();
    f.width = w;
    f.height = h;
    f.stride = w;
    f.codec = codec;
    f.frameRateN = 30;
    f.frameRateD = 1;
    f.aspectRatio = (float)w / (float)h;
    f.flags = VideoFlags::None;
    f.colorSpace = ColorSpace::BT709;
    f.timestamp = ts;
    return f;
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    omtqa::Tally& t = g_tally;
    omtqa::Stream s(45348, "qa-coded-send", t);

    std::vector<uint8_t> au(4096);
    for (size_t i = 0; i < au.size(); ++i) au[i] = (uint8_t)(i * 7 + 3);

    // ---- HX1/HX2: the payload arrives unchanged, under the HEVC tag -------------------------
    const int64_t ts1 = 1000000;
    s.sendDelivered(codedFrame(au, Codec::HEVC, 64, 64, ts1), t,
                    "HX1: a coded HEVC frame is delivered (send() returned the payload length)");
    omtqa::Wire w = s.reader->next();
    ok(w.got, "HX1b: a video record arrived");
    ok(w.ext.codec == Codec::HEVC, "HX1c: VideoExtHeader.codec is 'HEVC'");
    ok(w.payload.size() == au.size() && std::memcmp(w.payload.data(), au.data(), au.size()) == 0,
       "HX2: the payload is byte-identical to the access unit");

    // ---- HX3/HX4: geometry and timestamp travel with each frame ----------------------------
    std::vector<uint8_t> au2(1500, 0x5A);
    const int64_t ts2 = ts1 + 3333333;   // one 30 fps period in 100 ns ticks
    s.sendDelivered(codedFrame(au2, Codec::HEVC, 128, 72, ts2), t,
                    "HX3a: the second coded frame is delivered");
    omtqa::Wire w2 = s.reader->next();
    ok(w2.ext.width == 128 && w2.ext.height == 72,
       "HX3b: the header follows the frame's geometry (128x72, not the first frame's 64x64)");
    ok(w2.h.timestamp == ts2, "HX4a: the timestamp is carried in 100 ns ticks as given");
    ok(w2.h.timestamp - w.h.timestamp == 3333333,
       "HX4b: the timestamp delta is one 30 fps frame period");

    // ---- HX5: H.264 is the same pass-through under its own tag ----------------------------
    std::vector<uint8_t> au3(900, 0x11);
    s.sendDelivered(codedFrame(au3, Codec::H264, 96, 96, ts2 + 3333333), t,
                    "HX5a: a coded H.264 frame is delivered");
    omtqa::Wire w3 = s.reader->next();
    ok(w3.ext.codec == Codec::H264, "HX5b: VideoExtHeader.codec is 'H264'");

    // ---- HX6..HX8: what the coded path must refuse, counted and named like every other gate
    {
        const long long before = s.sender.encodeFailures();
        ok(s.sender.send(codedFrame(au, Codec::HEVC, 65, 64, ts1)) == 0,
           "HX6a: an odd width is refused (4:2:0 cannot tile it)");
        ok(s.sender.encodeFailures() == before + 1, "HX6b: and counted");
        ok(!s.sender.lastError().empty(), "HX6c: and named: \"" + s.sender.lastError() + "\"");

        MediaFrame empty = codedFrame(au, Codec::HEVC, 64, 64, ts1);
        empty.dataLength = 0;
        ok(s.sender.send(empty) == 0, "HX7a: an empty access unit is refused");
        ok(!s.sender.lastError().empty(), "HX7b: and named: \"" + s.sender.lastError() + "\"");

        MediaFrame alpha = codedFrame(au, Codec::HEVC, 64, 64, ts1);
        alpha.flags = VideoFlags::Alpha;
        ok(s.sender.send(alpha) == 0,
           "HX8a: Alpha claimed on a coded frame is refused — nothing in the payload carries a plane");
        ok(!s.sender.lastError().empty(), "HX8b: and named: \"" + s.sender.lastError() + "\"");
    }

    // ---- HX9: nobody watching is not a failure, same contract as the raw path --------------
    {
        SenderConfig c;
        c.name = "qa-coded-send-noclients";
        c.port = 45349;   // never started: this sender exists only to exercise the no-clients return
        Sender lonely(c);
        ok(lonely.send(codedFrame(au, Codec::HEVC, 64, 64, ts1)) == 1,
           "HX9a: with no subscriber, a coded frame returns 1 (not a refusal)");
        ok(lonely.encodeFailures() == 0, "HX9b: and is not counted as a failure");
    }

    // ---- HX10: the raw path is untouched (a sender that tagged everything HEVC fails here) --
    {
        std::vector<uint8_t> bgra((size_t)64 * 64 * 4, 0x3C);
        MediaFrame raw{};
        raw.data = bgra.data();
        raw.dataLength = (int)bgra.size();
        raw.width = 64;
        raw.height = 64;
        raw.stride = 64 * 4;
        raw.codec = Codec::BGRA;
        raw.frameRateN = 30;
        raw.frameRateD = 1;
        raw.aspectRatio = 1.0f;
        raw.flags = VideoFlags::None;
        raw.colorSpace = ColorSpace::BT709;
        raw.timestamp = ts1;
        s.sendDelivered(raw, t, "HX10a: a raw BGRA frame is still delivered");
        omtqa::Wire w4 = s.reader->next();
        ok(w4.ext.codec == Codec::VMX1, "HX10b: and is still tagged 'VMX1'");
    }

    return t.report("coded-send");
}