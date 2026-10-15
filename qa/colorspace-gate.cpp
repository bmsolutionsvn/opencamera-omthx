/*
 * colorspace-gate.cpp — the sender must refuse, by name, what the encoder cannot honour.
 *
 * MediaFrame::colorSpace is a plain int and VideoExtHeader::colorSpace is int32_t. The VMX codec
 * knows exactly three values (VMX_COLORSPACE_UNDEFINED/601/709 in libvmx/src/vmxcodec.h:84-88),
 * and the OMT wire spec defines more. Without a boundary check, a caller that passed 2020 got:
 * createEncoder() static_cast'ing 2020 straight into VMX_COLORSPACE (an out-of-range enum value),
 * and send() stamping 2020 into the header — so every receiver was told BT.2020 about a payload
 * encoded with whatever the codec fell back to. A same-codec loopback cannot detect that, because
 * both ends share the fallback; only the public boundary can refuse it.
 *
 * Part B covers the second half of the same lesson: an encode that produced nothing used to
 * return 0 in silence, invisible to every counter the UI reads, so a permanently failing codec
 * looked like an idle but healthy stream.
 *
 * Build + run:  bash qa/build-host.sh        (which also runs the mutation controls)
 */
#include "OMTSender.h"
#include "OMTProtocol.h"
#include "libomt.h"   // the public C enum: the authority these constants must match

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace omt;

static int failures = 0;
static void ok(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++failures;
}
static bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

static MediaFrame makeFrame(void* data, int codec, int colorSpace) {
    MediaFrame f{};
    f.data = data;
    f.width = 64; f.height = 64;            // even dims: VMX rejects odd ones
    f.stride = 64 * 4;
    f.dataLength = 64 * 64 * 4;
    f.codec = codec;
    f.frameRateN = 30; f.frameRateD = 1;    // frameRateD==0 would divide by zero in createEncoder
    f.aspectRatio = 1.0f;
    f.flags = 0;
    f.colorSpace = colorSpace;
    f.timestamp = 0;
    return f;
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::vector<uint8_t> pixels(64 * 64 * 4, 0x40);

    // ---------- A0: the wire numbering, which is the trap this whole file exists for ----------
    // VideoExtHeader::colorSpace carries OMTColorSpace values. If someone restores the old
    // 0/1/2/3 block in OMTProtocol.h, every receiver silently misreads the payload's primaries —
    // and the sender's own gate would then reject the correct values. Assert the numbers, not
    // just the behaviour.
    ok(omt::ColorSpace::Undefined == OMTColorSpace_Undefined &&
       omt::ColorSpace::BT601 == OMTColorSpace_BT601 &&
       omt::ColorSpace::BT709 == OMTColorSpace_BT709,
       "A0: the sender's colour-space constants equal the public OMTColorSpace enum (0/601/709)");
    static_assert(omt::ColorSpace::BT709 == OMTColorSpace_BT709,
                  "the internal constant drifted from the public enum — receivers would misread the payload");

    // ---------- A: the colour-space gate, exercised with no subscriber at all ----------------
    // The gate has to run before the "no clients -> return 1" shortcut, otherwise a stream with
    // nobody watching would accept a claim it can never honour and only fail once watched.
    SenderConfig cfg;
    cfg.name = "qa-colorspace";
    cfg.port = 45331;
    cfg.quality = Quality::Default;
    Sender sender(cfg);

    ok(sender.send(makeFrame(pixels.data(), Codec::BGRA, 2020)) == 0,
       "A1: a colour space the codec cannot encode is refused with a failure code");
    ok(has(sender.lastError(), "unsupported colorspace 2020"),
       "A2: and it says which one, by name, instead of failing silently");

    ok(sender.send(makeFrame(pixels.data(), Codec::BGRA, 1)) == 0 &&
       has(sender.lastError(), "unsupported colorspace 1"),
       "A3: a value that is not even in the enum is refused too (it is an int, nothing type-checks it)");

    // The accepted set: what VMX actually encodes. Each must reach the normal no-clients path.
    for (int cs : {0, 601, 709}) {
        int r = sender.send(makeFrame(pixels.data(), Codec::BGRA, cs));
        ok(r == 1, (std::string("A4: colorspace ") + std::to_string(cs) +
                    " is accepted (returns the no-subscribers code, not a failure)").c_str());
    }

    // A caller that leaves the framerate denominator at zero must not take the process down:
    // createEncoder() computes frameRateN / frameRateD.
    {
        MediaFrame f = makeFrame(pixels.data(), Codec::BGRA, 709);
        f.frameRateD = 0;
        ok(sender.send(f) == 0, "A5: frameRateD == 0 is refused rather than dividing by zero");
    }

    // ---------- B: an encode that yields nothing must be counted and named -------------------
    // Needs one connected subscriber, because the no-clients path returns before encoding.
    if (!sender.start()) {
        std::printf("  [FAIL] could not start the sender on port %d\n", cfg.port);
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

    const long before = sender.encodeFailures();
    ok(sender.send(makeFrame(pixels.data(), 0x7FA0BADC /* not a real codec */, 709)) == 0,
       "B1: an unknown codec is refused");
    ok(sender.encodeFailures() == before + 1,
       "B2: and the failure is COUNTED — a codec that keeps failing must show up in the numbers");
    ok(has(sender.lastError(), "encoder produced no payload"),
       "B3: with a reason an operator can read without a debugger");

    close(fd);
    sender.stop();

    std::printf("\ncolorspace-gate: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
