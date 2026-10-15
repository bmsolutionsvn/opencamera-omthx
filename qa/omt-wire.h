/*
 * omt-wire.h — the subscriber side of the sender QA scenarios, shared by qa/encoder-reconfiguration.cpp
 * and qa/wire-declaration.cpp.
 *
 * Both files need the same three things: a Sender that is really streaming to a real socket, a
 * subscriber that sent the OMT video-subscribe control (Sender::send() skips a channel that has not,
 * so an unsubscribed socket exercises none of the fan-out path), and a parser that turns the bytes
 * back into FrameHeader + VideoExtHeader + payload. The parser is deliberately independent of the
 * sender's own bookkeeping: it reads what a receiver would read, which is the only way a header that
 * disagrees with its payload can be caught rather than agreed with.
 *
 * A record on the wire is FrameHeader + dataLength bytes. For video, dataLength is the ext-header
 * plus the payload and metadataLength is 0; for metadata (the <OMTInfo> the channel pushes on
 * connect, the answer to a subscribe) Channel::sendMetadataSync writes sizeof(FrameHeader)+totalLen
 * with BOTH fields set to totalLen — the metadata is the data. Adding the two fields would
 * double-count and desynchronise the parse; that mistake made an earlier revision of these tests
 * receive nothing and pass anyway.
 */
#ifndef QA_OMT_WIRE_H
#define QA_OMT_WIRE_H

#include "OMTConstants.h"
#include "OMTProtocol.h"
#include "OMTSender.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace omtqa {

using omt::MediaFrame;
using omt::Sender;
using omt::SenderConfig;

struct Tally {
    int failures = 0;
    void ok(bool cond, const std::string& what) {
        std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
        if (!cond) ++failures;
    }
    static bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }
    int report(const char* name) {
        std::printf("\n%s: %s (%d failures)\n", name, failures ? "FAILURES" : "ALL PASS", failures);
        return failures ? 1 : 0;
    }
};

struct Wire {
    bool got = false;
    omt::FrameHeader h{};
    omt::VideoExtHeader ext{};
    std::vector<uint8_t> payload;
};

class Reader {
public:
    explicit Reader(int fd) : fd_(fd) {}
    Wire next(int timeoutMs = 3000) {
        Wire w;
        for (;;) {
            if (!fill(16, timeoutMs)) { setupFailure = "no header within the timeout"; return w; }
            omt::FrameHeader h{};
            std::memcpy(&h, buf_.data(), sizeof(h));
            if (h.version != 1 || h.dataLength <= 0 ||
                h.dataLength > omt::Constants::VIDEO_MAX_SIZE) {
                setupFailure = "unparseable record header";
                return w;
            }
            const size_t need = omt::FrameHeader::SIZE + (size_t)h.dataLength;
            if (!fill(need, timeoutMs)) { setupFailure = "record cut short"; return w; }
            omt::FrameHeader hh{};
            std::memcpy(&hh, buf_.data(), sizeof(hh));
            const size_t total = omt::FrameHeader::SIZE + (size_t)hh.dataLength;
            if (hh.frameType == (uint8_t)omt::FrameType::Video &&
                hh.dataLength >= (int32_t)omt::VideoExtHeader::SIZE) {
                w.got = true;
                w.h = hh;
                std::memcpy(&w.ext, buf_.data() + omt::FrameHeader::SIZE, sizeof(w.ext));
                const size_t plen = (size_t)hh.dataLength - omt::VideoExtHeader::SIZE;
                w.payload.assign(buf_.begin() + omt::FrameHeader::SIZE + omt::VideoExtHeader::SIZE,
                                 buf_.begin() + omt::FrameHeader::SIZE + omt::VideoExtHeader::SIZE + plen);
                buf_.erase(buf_.begin(), buf_.begin() + total);
                return w;
            }
            buf_.erase(buf_.begin(), buf_.begin() + total);   // metadata or anything else not video
        }
    }
    std::string setupFailure;   // non-empty => the harness broke, not the code under test
private:
    bool fill(size_t n, int timeoutMs) {
        while (buf_.size() < n) {
            struct pollfd p{fd_, POLLIN, 0};
            int pr = ::poll(&p, 1, timeoutMs);
            if (pr <= 0) return false;
            char tmp[65536];
            ssize_t r = ::recv(fd_, tmp, sizeof(tmp), 0);
            if (r <= 0) return false;
            buf_.insert(buf_.end(), tmp, tmp + r);
        }
        return true;
    }
    int fd_;
    std::vector<uint8_t> buf_;
};

// A sender with one connected, video-subscribed subscriber. Every field of the setup is asserted
// through the Tally it is handed: a scenario that silently lost its channel would otherwise score
// "delivered" from Sender::send()'s no-clients return of 1 and compare nothing at all.
struct Stream {
    Sender sender;
    int fd = -1;
    Reader* reader = nullptr;

    static SenderConfig cfg(int port, const char* name) {
        SenderConfig c; c.name = name; c.port = port; c.quality = omt::Quality::Default; return c;
    }

    Stream(int port, const char* name, Tally& t) : sender(cfg(port, name)) {
        if (!sender.start()) { t.ok(false, std::string("SETUP: ") + name + " sender.start() on port " +
                                                std::to_string(port)); return; }
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (::connect(fd, (sockaddr*)&a, sizeof(a)) != 0) {
            t.ok(false, std::string("SETUP: ") + name + " connect to port " + std::to_string(port));
            return;
        }
        for (int i = 0; i < 150 && sender.connectionCount() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        t.ok(sender.connectionCount() == 1,
            std::string("SETUP: ") + name + " registered exactly one channel");
        const char* xml = omt::MetadataConstants::CHANNEL_SUBSCRIBE_VIDEO;
        const size_t xlen = std::strlen(xml) + 1;
        std::vector<uint8_t> m(omt::FrameHeader::SIZE + xlen, 0);
        omt::FrameHeader h{};
        h.version = 1; h.frameType = (uint8_t)omt::FrameType::Metadata;
        h.metadataLength = (uint16_t)xlen; h.dataLength = (int32_t)xlen;
        std::memcpy(m.data(), &h, sizeof(h));
        std::memcpy(m.data() + omt::FrameHeader::SIZE, xml, xlen);
        t.ok(::send(fd, m.data(), m.size(), MSG_NOSIGNAL) == (ssize_t)m.size(),
             std::string("SETUP: ") + name + " subscriber sent <OMTSubscribe Video=\"true\" />");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));  // let receiverLoop process it
        reader = new Reader(fd);
    }
    ~Stream() { delete reader; if (fd >= 0) ::close(fd); sender.stop(); }

    // True only if the frame really went to a subscriber: send() returns the encoded length then,
    // and exactly 1 when there is nobody to send it to.
    bool sendDelivered(const MediaFrame& f, Tally& t, const std::string& what) {
        const int r = sender.send(f);
        t.ok(r > 1, what + " — reached a subscriber (send() returned " + std::to_string(r) +
                    "; 1 would mean no client was attached and 0 a refusal)");
        return r > 1;
    }
};

} // namespace omtqa

#endif // QA_OMT_WIRE_H
