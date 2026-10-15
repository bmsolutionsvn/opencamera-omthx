/*
 * encoder-create-failure.cpp — when the codec refuses to build an encoder, the sender must say so,
 * count it, and stay able to stream.
 *
 * The defect (found while porting P0 #9): createEncoder() called VMX_Create(), ignored the NULL it
 * can return, and cached currentWidth_/currentHeight_ anyway. Two things followed. acceptLoop()
 * sizes every new subscriber's async pool from that cache, so it was advertising buffers for a
 * frame that does not exist; and the only thing the operator could read was the generic "encoder
 * produced no payload", which is a different failure with a different cause. The fix names the
 * failure and writes the cache only once an encoder actually exists.
 *
 * What this file does NOT claim: the brief said the stale cache made "every later frame at that
 * size take the already-have-correct-encoder early-out and return 0 forever". It does not — that
 * early-out also tests vmxInstance_, which is null in exactly this case, so the sender retried the
 * create every frame. D5/D6 below pin that (no lockout, recovery as soon as the create succeeds) in
 * both builds, so if the early-out is ever simplified the suite notices; the assertion that
 * discriminates is D2, which is about the name of the failure.
 *
 * The real VMX_Create cannot return NULL for a frame that passes validateFrame() — both quote
 * vmxcodec_common.h — so this scenario links qa/stub_vmx.cpp instead of the codec.
 *
 * Build + run:  bash qa/build-host.sh   (which also runs the mutation control)
 */
#include "OMTSender.h"
#include "OMTProtocol.h"

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

// The recorder side of qa/stub_vmx.cpp.
namespace qa_stub {
    extern int failWidth, failHeight;
    extern int payloadBytes;
    extern int createCalls, destroyCalls, encodeCalls, saveCalls;
    void reset();
}

static int failures = 0;
static void ok(bool cond, const std::string& what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++failures;
}
static bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

static MediaFrame bgra(std::vector<uint8_t>& px, int w, int h, int64_t ts) {
    MediaFrame f{};
    f.data = px.data();
    f.dataLength = (int)px.size();
    f.width = w;
    f.height = h;
    f.stride = w * 4;
    f.codec = Codec::BGRA;
    f.frameRateN = 30;
    f.frameRateD = 1;
    f.aspectRatio = (float)w / (float)h;
    f.flags = 0;
    f.colorSpace = ColorSpace::BT709;
    f.timestamp = ts;
    return f;
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);

    SenderConfig cfg;
    cfg.name = "qa-encoder-create";
    cfg.port = 45335;
    cfg.quality = Quality::Default;
    Sender sender(cfg);
    if (!sender.start()) {
        std::printf("  [FAIL] sender.start() on port %d\n", cfg.port);
        return 1;
    }

    // A subscriber that subscribes, so the encode really is attempted and fanned out.
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(cfg.port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    ok(::connect(fd, (sockaddr*)&a, sizeof(a)) == 0, "a subscriber socket connected");
    for (int i = 0; i < 100 && sender.connectionCount() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ok(sender.connectionCount() == 1, "the sender accepted it as a channel");
    {
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

    // 1088 is a legal frame height — validateFrame() admits it and VMX would too. The stub is what
    // says no, standing in for the allocation/creation failure that the real codec cannot be driven
    // into from this side of the gate.
    const int W = 640, H = 1088;
    std::vector<uint8_t> px((size_t)W * H * 4);
    uint32_t rng = 0xBADF00Du;
    for (auto& b : px) { rng = rng * 1103515245u + 12345u; b = (uint8_t)(rng >> 16); }

    qa_stub::reset();
    qa_stub::failWidth = W;
    qa_stub::failHeight = H;
    const long long before = sender.encodeFailures();

    int r = sender.send(bgra(px, W, H, 1));
    ok(r == 0, "D1: a frame whose encoder could not be created is refused (returned " + std::to_string(r) + ")");
    ok(has(sender.lastError(), "VMX_Create refused 640x1088"),
       "D2: and it is named as an encoder-creation failure, not as the generic empty-payload one: \"" +
       sender.lastError() + "\"");
    ok(sender.encodeFailures() == before + 1,
       "D3: and it is counted, so a codec that cannot come up is visible in the numbers");
    ok(qa_stub::createCalls == 1 && qa_stub::encodeCalls == 0,
       "D4: it tried to create the encoder once and never handed the frame to a null instance "
       "(createCalls=" + std::to_string(qa_stub::createCalls) + ", encodeCalls=" +
       std::to_string(qa_stub::encodeCalls) + ")");

    // D5/D6: the claim in the brief that does NOT hold here — the stale size cache used to be
    // described as locking the stream out forever. It does not, because the
    // "already have correct encoder" early-out also tests vmxInstance_. Pinned anyway: if anyone
    // simplifies that condition, the retry disappears and this goes red.
    r = sender.send(bgra(px, W, H, 2));
    ok(r == 0 && qa_stub::createCalls == 2,
       "D5: the next frame at the same size tries again rather than trusting a cached size with no "
       "encoder behind it (createCalls=" + std::to_string(qa_stub::createCalls) + ")");
    ok(has(sender.lastError(), "VMX_Create refused"), "D6: and it still says why");

    // D7: recovery. The moment the encoder can be built, the same frames must flow — a failed
    // create must not leave the sender believing it already has an encoder for that size.
    qa_stub::failWidth = qa_stub::failHeight = 0;
    r = sender.send(bgra(px, W, H, 3));
    ok(r > 0, "D7: once VMX_Create succeeds, the same 640x1088 frame goes out (" + std::to_string(r) +
              " bytes) — the failure did not poison the size cache");

    // D8: the generic message still belongs to the case it was written for: an encoder that exists
    // and produces nothing. (This is the assertion qa/colorspace-gate.cpp used to make with an
    // unknown codec, which validateFrame now catches earlier and by name.)
    const long long before8 = sender.encodeFailures();
    qa_stub::payloadBytes = 0;                       // the encoder exists, and yields nothing
    std::vector<uint8_t> px2((size_t)(W + 32) * H * 4);   // a new size: forces a fresh create + encode
    for (auto& b : px2) { rng = rng * 1103515245u + 12345u; b = (uint8_t)(rng >> 16); }
    r = sender.send(bgra(px2, W + 32, H, 4));
    ok(r == 0, "D8: an encoder that produces no payload is refused");
    ok(has(sender.lastError(), "encoder produced no payload"),
       "D9: with the message that describes THAT failure: \"" + sender.lastError() + "\"");
    ok(sender.encodeFailures() == before8 + 1, "D10: and counted");

    close(fd);
    sender.stop();

    std::printf("\nencoder-create-failure: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
