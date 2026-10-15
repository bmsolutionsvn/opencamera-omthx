/*
 * drop-policy.cpp — when the ring is full, which frame survives?
 *
 * The per-subscriber ring is the AsyncPool: 4 buffers of 1 MB (OMTConstants.h NETWORK_ASYNC_COUNT
 * / NETWORK_ASYNC_BUFFER_AV), fixed at compile time. The original policy was "no free buffer ->
 * drop the frame in hand", which keeps the OLDEST queued frames. On a live stream that is
 * backwards: after a brief stall the sender replays up to 4 already-decoded frames (133 ms at
 * 30 fps) before any live picture appears, so a stall becomes latency instead of a few lost
 * frames.
 *
 * The subscriber here is a real OMT client: it sends the <OMTSubscribe Video="true"/> control
 * frame (Sender::send only fans out to video subscribers, so an unsubscribed socket would exercise
 * none of this), stops reading until the ring is saturated, then drains. The bytes are parsed back
 * into frame timestamps:
 *
 *   drop-oldest (fixed) -> the newest frame is present, some of the oldest are missing
 *   drop-newest (before)-> the newest frame never arrives at all
 *
 * Frames are incompressible noise at 512x512 (~200 KB each): a flat image encodes so small that
 * the kernel buffers on a loopback link absorb the whole burst and the TCP window never closes,
 * which would let the test pass without touching the ring.
 *
 * Build + run: bash qa/build-host.sh  (which also runs it with the policy disabled and the
 * drop counter stubbed back to zero — both must turn this file red).
 */
#include "OMTSender.h"
#include "OMTProtocol.h"
#include "OMTConstants.h"
#include "libomt.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace omt;

static int failures = 0;
static void ok(bool cond, const std::string& what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++failures;
}

// Pull the video frame timestamps back out of the byte stream the sender produced.
static std::set<int64_t> parseTimestamps(const std::vector<uint8_t>& s) {
    std::set<int64_t> ts;
    size_t i = 0;
    while (i + FrameHeader::SIZE <= s.size()) {
        FrameHeader h;
        memcpy(&h, s.data() + i, sizeof(h));
        if (h.version != 1 || h.frameType != static_cast<uint8_t>(FrameType::Video) ||
            h.dataLength <= 0 || h.dataLength > Constants::VIDEO_MAX_SIZE) { ++i; continue; }
        ts.insert(h.timestamp);
        i += FrameHeader::SIZE + (size_t)h.dataLength;
    }
    return ts;
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);

    SenderConfig cfg; cfg.name = "qa-drop"; cfg.port = 45332; cfg.quality = Quality::Default;
    Sender sender(cfg);
    if (!sender.start()) { std::printf("  [FAIL] sender.start() on port %d\n", cfg.port); return 1; }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(cfg.port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::connect(fd, (sockaddr*)&a, sizeof(a)) != 0) { std::printf("  [FAIL] connect\n"); return 1; }
    for (int i = 0; i < 100 && sender.connectionCount() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ok(sender.connectionCount() == 1, "one subscriber attached");

    // Subscribe like a real receiver: without this Sender::send skips the channel entirely and the
    // ring is never touched — the test would pass while proving nothing.
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
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // let receiverLoop process it

    std::vector<uint8_t> pixels(512 * 512 * 4);
    uint32_t rng = 0x12345678u;
    for (auto& b : pixels) { rng = rng * 1103515245u + 12345u; b = (uint8_t)(rng >> 16); }
    auto push = [&](int64_t index) {
        MediaFrame f{};
        f.data = pixels.data(); f.width = 512; f.height = 512; f.stride = 512 * 4;
        f.dataLength = 512 * 512 * 4; f.codec = Codec::BGRA;
        f.frameRateN = 30; f.frameRateD = 1; f.aspectRatio = 1.0f;
        f.colorSpace = ColorSpace::BT709; f.timestamp = index;
        return sender.send(f);
    };

    int64_t dropped = 0; int pushed = 0;
    for (int i = 1; i <= 120 && dropped == 0; ++i) {
        push(i); ++pushed;
        dropped = sender.getRecentDropsAndReset();
    }
    std::printf("      pushed %d frames before the ring reported %lld drop(s)\n", pushed, (long long)dropped);
    ok(dropped > 0, "A: a saturated ring reports drops — the congestion alert in OMTStreamingManager "
                    "is gated on this value, and omt_send_get_recent_drops_and_reset() used to return "
                    "a hardcoded 0, making that UI feature unreachable code");
    ok(sender.getRecentDropsAndReset() == 0,
       "B: it is a window counter, not a running total — a second read with no new drops is zero");

    // Drain everything the subscriber managed to receive.
    std::vector<uint8_t> got;
    {
        int off = 0; (void)off;
        uint8_t buf[65536];
        auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4)) {
            fd_set r; FD_ZERO(&r); FD_SET(fd, &r);
            timeval tv{0, 100000};
            if (select(fd + 1, &r, nullptr, nullptr, &tv) <= 0) break;
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            got.insert(got.end(), buf, buf + n);
        }
    }
    close(fd);
    sender.stop();

    std::set<int64_t> got_ts = parseTimestamps(got);
    ok(!got_ts.empty(), "C: the subscriber received at least one complete video frame");
    ok(got_ts.count(pushed) == 1,
       "D: the NEWEST frame survived — under the original drop-newest policy the frame in hand was "
       "the one discarded, so the last frame pushed never reached the wire");
    int missing_oldest = 0;
    for (int64_t i = 1; i <= std::min<int64_t>(pushed, 3); ++i) if (!got_ts.count(i)) ++missing_oldest;
    ok(missing_oldest > 0,
       "E: and what was lost were the OLDEST frames — a live viewer has no use for a replayed backlog");
    std::printf("      received %zu distinct frame ids of %d pushed\n", got_ts.size(), pushed);

    std::printf("\ndrop-policy: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
