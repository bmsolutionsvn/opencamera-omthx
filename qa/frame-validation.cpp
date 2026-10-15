/*
 * frame-validation.cpp — the sender must refuse a frame the buffer cannot honour, by name,
 * before it allocates an encoder or hands that buffer to the codec.
 *
 * Why this exists (P0 #9 / P0 #16, ported from the desktop vendored sender):
 *
 * Java OMTSender.sendFrame(buffer, w, h, yStride, uvStride) reaches
 * app/src/main/cpp/OMTBridge.cpp::nativeSendFrame, which computes
 *
 *     dataLength = height*yStride + (height/2)*uvStride
 *
 * and then puts ONE of those two strides into OMTMediaFrame::Stride. libomt_android.cpp copies the
 * struct verbatim with no checks, and Sender::encodeFrame() passes that single value to VMX as both
 * srcStrideY and srcStrideUV. So the encoder walks the chroma plane at
 *
 *     data + height*yStride + row*yStride        for row in [0, height/2)
 *
 * — it needs (height/2)*yStride bytes after the luma plane, while the ImageReader only reserved
 * (height/2)*uvStride. Camera2 exposes the two row strides separately and many devices pad them
 * differently, so on such a device the last chroma rows read past the end of the allocation:
 * a SIGSEGV in the camera process, on an untested phone, with no log line and no counter. Nothing
 * between the JNI entry point and the codec looked at any of it.
 *
 * Part A exercises the gate clause by clause with no subscriber and no allocation.
 * Part B is the actual out-of-bounds read: a buffer sized exactly the way the JNI bridge sizes one
 * (uvStride < yStride), under ASan. Remove the gate and this binary dies with a heap-buffer-overflow
 * instead of printing a verdict — that A/B (see qa/build-host.sh) is what makes it evidence.
 * Part C pins the encode-buffer arithmetic that used to be `int maxSize = width * height * 4`.
 *
 * Build + run:  bash qa/build-host.sh   (which also runs the mutation controls)
 */
#include "OMTSender.h"
#include "OMTProtocol.h"
#include "libomt.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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

static MediaFrame baseFrame(void* data, int codec, int w, int h, int stride, int dataLength) {
    MediaFrame f{};
    f.data = data;
    f.dataLength = dataLength;
    f.width = w;
    f.height = h;
    f.stride = stride;
    f.codec = codec;
    f.frameRateN = 30;
    f.frameRateD = 1;
    f.aspectRatio = 1.0f;
    f.flags = 0;
    f.colorSpace = ColorSpace::BT709;
    f.timestamp = 0;
    return f;
}

// The shape nativeSendFrame() produces for a Camera2 device whose two row strides differ.
struct Nv12Padded {
    int w = 256, h = 64, yStride = 320, uvStride = 256;
    std::vector<uint8_t> buf;
    Nv12Padded() {
        // Exactly what OMTBridge.cpp asks for: h*yStride + (h/2)*uvStride. ASan instruments this
        // allocation, so the encoder's chroma walk running past its end is a report, not a rumour.
        buf.resize((size_t)h * yStride + (size_t)(h / 2) * uvStride);
        uint32_t rng = 0xC0FFEE11u;
        for (auto& b : buf) { rng = rng * 1103515245u + 12345u; b = (uint8_t)(rng >> 16); }
    }
    int dataLength() const { return (int)buf.size(); }
    MediaFrame frame() {
        MediaFrame f = baseFrame(buf.data(), Codec::NV12, w, h, yStride, dataLength());
        return f;
    }
};

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::vector<uint8_t> dummy(1 << 20, 0x40);   // validateFrame never dereferences data
    std::string why;

    // A helper that asserts BOTH halves of a rejection: the verdict and the reason. A gate that
    // refuses without saying why is the bug b66ab347 fixed for the encoder, re-introduced here.
    auto rejects = [&](const MediaFrame& f, const char* needle, const std::string& what) {
        why.clear();
        bool valid = Sender::validateFrameForTest(f, why);
        bool good = !valid && has(why, needle);
        ok(good, what + (good ? std::string() : std::string("   [verdict=") + (valid ? "accepted" : "refused") +
                         ", why=\"" + why + "\", expected the reason to contain \"" + needle + "\")"));
    };
    auto accepts = [&](const MediaFrame& f, const std::string& what) {
        why.clear();
        bool valid = Sender::validateFrameForTest(f, why);
        ok(valid, what + (valid ? std::string() : std::string("   [refused: \"") + why + "\"]"));
    };

    // ---------- A0: the gate must not reject what the camera actually produces ----------
    // Without this, every rejection below is vacuous: a validateFrame() that always returns false
    // would "catch" the out-of-bounds frame and break the stream in the bargain.
    {
        MediaFrame f = baseFrame(dummy.data(), Codec::NV12, 256, 64, 320, 64 * 320 + 32 * 320);
        accepts(f, "A0a: a tightly-described NV12 frame (both planes at the stride the encoder uses) is accepted");
        MediaFrame b = baseFrame(dummy.data(), Codec::BGRA, 512, 512, 512 * 4, 512 * 512 * 4);
        accepts(b, "A0b: the BGRA frame the existing QA suites send is accepted");
        MediaFrame p = baseFrame(dummy.data(), Codec::UYVY, 64, 64, 128, 63 * 128 + 128);
        accepts(p, "A0c: a packed 4:2:2 frame with no padding is accepted (need = (h-1)*stride + row)");
    }

    // ---------- A: the clauses, one at a time ----------
    {
        MediaFrame f = baseFrame(nullptr, Codec::NV12, 256, 64, 320, 64 * 320 + 32 * 320);
        rejects(f, "null data", "A1: a null data pointer is refused before anything dereferences it");

        f = baseFrame(dummy.data(), Codec::NV12, 8, 64, 320, 64 * 320 + 32 * 320);
        rejects(f, "width 8", "A2: a width below the codec's minimum is refused (VMX_Create would return NULL)");
        f = baseFrame(dummy.data(), Codec::NV12, 7682, 64, 7682, 64 * 7682 + 32 * 7682);
        rejects(f, "7682", "A3: a width above the codec's maximum is refused");
        f = baseFrame(dummy.data(), Codec::NV12, 63, 64, 64, 64 * 64 + 32 * 64);
        rejects(f, "odd width 63", "A4: an odd width is refused (VMX_Create rejects it outright)");
        f = baseFrame(dummy.data(), Codec::NV12, 256, 8, 320, 8 * 320 + 4 * 320);
        rejects(f, "height 8", "A5: a height below the codec's minimum is refused");
        f = baseFrame(dummy.data(), Codec::NV12, 256, 4322, 256, 4322 * 256 + 2161 * 256);
        rejects(f, "4322", "A6: a height above the codec's maximum is refused");

        f = baseFrame(dummy.data(), Codec::NV12, 256, 64, 0, 64 * 320 + 32 * 256);
        rejects(f, "stride 0", "A7: stride 0 is refused — the encoder adds stride*row to the data pointer");
        f = baseFrame(dummy.data(), Codec::NV12, 256, 64, -320, 64 * 320 + 32 * 256);
        rejects(f, "stride -320", "A8: a negative stride is refused for the same reason");

        f = baseFrame(dummy.data(), Codec::NV12, 256, 64, 320, -1);
        rejects(f, "negative", "A9: a negative dataLength is refused");

        f = baseFrame(dummy.data(), Codec::BGRA, 512, 512, 2047, 512 * 2048);
        rejects(f, "narrower than one BGRA row", "A10: a stride narrower than one pixel row is refused (BGRA needs width*4)");
        f = baseFrame(dummy.data(), Codec::UYVY, 64, 64, 127, 64 * 128);
        rejects(f, "narrower than one UYVY row", "A11: same for packed 4:2:2 (needs width*2)");
        f = baseFrame(dummy.data(), Codec::NV12, 256, 64, 200, 64 * 256 + 32 * 256);
        rejects(f, "cannot hold a luma row", "A12: NV12 with stride < width is refused");

        f = baseFrame(dummy.data(), Codec::BGRA, 64, 64, 256, 64 * 256 - 1);
        rejects(f, "buffer too small", "A13: one byte short of the last row is refused (the codec would read it anyway)");
        f = baseFrame(dummy.data(), Codec::NV12, 256, 65, 320, 65 * 320 + 32 * 320);
        rejects(f, "odd height 65", "A14: NV12 at an odd height is refused — chroma is height/2 rows");

        f = baseFrame(dummy.data(), 0x7FA0BADC, 256, 64, 320, 64 * 320 + 32 * 320);
        rejects(f, "unknown codec 0x7FA0BAD", "A15: a codec this sender cannot encode is refused by fourCC, "
                                              "not discovered later in encodeFrame's default branch");

        f = baseFrame(dummy.data(), Codec::NV12, 256, 64, 320, 64 * 320 + 32 * 320);
        f.frameRateD = 0;
        rejects(f, "frameRateD is 0", "A16: frameRateD == 0 is refused (it is a divisor twice over in send())");
        f = baseFrame(dummy.data(), Codec::NV12, 256, 64, 320, 64 * 320 + 32 * 320);
        f.frameRateD = -1;
        rejects(f, "frameRateD is -1", "A17: a negative denominator is invalid too");
        f = baseFrame(dummy.data(), Codec::NV12, 256, 64, 320, 64 * 320 + 32 * 320);
        f.frameRateN = 0;
        rejects(f, "frameRateN is 0", "A18 (P0 #16): a zero frame-rate numerator is refused — it would be "
                                       "stamped into VideoExtHeader and read by every receiver");
        f = baseFrame(dummy.data(), Codec::NV12, 256, 64, 320, 64 * 320 + 32 * 320);
        f.frameRateN = -30;
        rejects(f, "frameRateN is -30", "A19 (P0 #16): and a negative one");

        f = baseFrame(dummy.data(), Codec::NV12, 256, 64, 320, 64 * 320 + 32 * 320);
        f.colorSpace = 2020;
        rejects(f, "unsupported colorspace 2020", "A20: the colour-space clause came along from the desktop "
                                                  "gate, in this tree's names (ColorSpace::Undefined, not Unknown) "
                                                  "so the 0/601/709 pin keeps its meaning");
    }

    // ---------- A21: the defect itself, as the JNI bridge builds it ----------
    Nv12Padded padded;
    {
        MediaFrame f = padded.frame();
        rejects(f, "buffer too small for NV12 256x64 stride=320",
                "A21: the Camera2 shape — dataLength sized with uvStride, planes walked with yStride — "
                "is refused here rather than read");
        ok(has(why, "uv rowStride"), "A22: and the message says which stride, so the next person to meet "
                                     "it does not have to rediscover the plane arithmetic");
    }

    // ---------- B: end-to-end under ASan, with a real subscriber ----------
    SenderConfig cfg;
    cfg.name = "qa-frame-validation";
    cfg.port = 45334;
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
    {   // Subscribe, or Sender::send skips the channel and the encode is never fanned out.
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

    const long long before = sender.encodeFailures();

    // B1: the out-of-bounds frame. Reaching the next line at all is part of the assertion — in the
    // reverted build the encoder reads past the allocation and ASan kills the process here.
    {
        int r = sender.send(padded.frame());
        ok(r == 0, "B1: the padded NV12 frame is refused with a failure code (and the process is still "
                   "alive — without the gate this line is an ASan heap-buffer-overflow READ)");
        ok(has(sender.lastError(), "buffer too small for NV12 256x64 stride=320"),
           "B2: it names the size, the stride and the shortfall: " + sender.lastError());
        ok(sender.encodeFailures() == before + 1,
           "B3: and it is COUNTED, so a device that does this on every frame shows up in the numbers "
           "instead of looking like an idle but healthy stream");
    }

    // B4: the same frame with the strides it should have had must still go out, otherwise B1 only
    // proves the sender is broken rather than that it is careful.
    {
        std::vector<uint8_t> good((size_t)padded.h * padded.yStride +
                                  (size_t)(padded.h / 2) * padded.yStride, 0x3C);
        uint32_t rng = 0x5eed1234u;
        for (auto& b : good) { rng = rng * 1103515245u + 12345u; b = (uint8_t)(rng >> 16); }
        MediaFrame f = baseFrame(good.data(), Codec::NV12, padded.w, padded.h, padded.yStride, (int)good.size());
        int r = sender.send(f);
        ok(r > 0, "B4: a correctly described NV12 frame at the same dimensions still encodes and reaches "
                  "the subscriber (returned " + std::to_string(r) + " bytes) — the gate is selective");
        ok(sender.encodeFailures() == before + 1,
           "B5: and it is not counted as a failure, so the counter keeps meaning what it says");
    }

    close(fd);
    sender.stop();

    // ---------- C: the encode-buffer arithmetic ----------
    // createEncoder() used to compute `int maxSize = width * height * 4` and clamp it to 1 MB if it
    // came out small. At 32768x16385 the signed multiply wraps to a negative number, the clamp
    // "repairs" it to 1 MB, and the buffer has nothing to do with the frame. validateFrame bounds the
    // dimensions today, so this pins the arithmetic underneath the gate rather than a live path.
    {
        volatile int w = 32768, h = 16385;   // runtime operands: not constant-foldable
        size_t cap = Sender::encodeBufferCapacityForTest(w, h);
        size_t ceiling = (size_t)7680 * 4320 * 4;
        ok(cap == ceiling, "C1: a width*height*4 that overflows int is clamped to the documented "
                           "ceiling (" + std::to_string(cap) + "), never wrapped into a negative and "
                           "'repaired' to the 1 MB floor");

        volatile int w2 = 1920, h2 = 1080;
        size_t cap2 = Sender::encodeBufferCapacityForTest(w2, h2);
        ok(cap2 == (size_t)1920 * 1080 * 4, "C2: a normal frame gets width*height*4, exactly "
                                            "(" + std::to_string(cap2) + " bytes)");

        volatile int w3 = 64, h3 = 64;
        size_t cap3 = Sender::encodeBufferCapacityForTest(w3, h3);
        ok(cap3 == 1024u * 1024u, "C3: the 1 MB floor is still there for tiny frames (" +
                                  std::to_string(cap3) + " bytes)");

        volatile int w4 = 7680, h4 = 4320;
        size_t cap4 = Sender::encodeBufferCapacityForTest(w4, h4);
        ok(cap4 == (size_t)7680 * 4320 * 4, "C4: the largest admissible frame sits exactly on the "
                                            "ceiling, so the clamp never undersizes a legal frame");
    }

    std::printf("\nframe-validation: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
