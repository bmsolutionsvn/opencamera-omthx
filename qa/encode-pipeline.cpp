/*
 * encode-pipeline.cpp — the ImageReader callback may not wait for the codec.
 *
 * WHAT IS UNDER TEST, AND WHY IT IS THE REAL THING
 * app/src/main/cpp/OMTBridge.cpp is compiled into this binary — the production translation unit, not
 * a transcription — against qa/host-jni/jni.h, and the test calls its
 * Java_net_sourceforge_opencamera_OMTSender_nativeQueueFramePlanes() exactly as the ImageReader
 * callback in OMTStreamingManager.java does. Behind the bridge sits the real omt::Sender and the real
 * omtpack::packNv12(); the two substituted parts are the JNIEnv, which the test owns, and the codec,
 * which comes from qa/stub_vmx.cpp at LINK time with its encode set to sleep 70 ms
 * (qa_stub::encodeDelayUs). 70 ms is not a made-up number: it is the order of magnitude that turned
 * the callback into the frame-rate limiter — the same build delivered 30.4/30.2 fps natively on the
 * x86_64 emulator and 11-13 fps under arm64 translation, and delivered fps is 1/(callback cost).
 *
 * THE DEFECT THIS MEASURES. The callback used to run pack + encode + headers inline, on the
 * ImageReader's thread, with maxImages=3. The camera cannot recycle a buffer until the callback
 * returns, so the delivered rate was whatever that callback could chew through. Nothing in the code
 * said the callback had to be cheap, and nothing on the host could notice when it wasn't — which is
 * why the ~14 fps report survived every suite that ran before this file existed.
 *
 * WHAT EACH CASE IS THE EVIDENCE FOR:
 *   E1  the callback returns in microseconds while an encode takes 70 ms. This is the decoupling.
 *   E2  what arrives at the sender is the frame that went in, in the order it went in, byte for byte
 *       against the packer's own oracle — and each one's destination buffer is a different buffer.
 *   E3  when the producer outruns the encoder the excess is EVICTED AND COUNTED, not accumulated and
 *       not silently lost: admitted == delivered + dropped, and dropped > 0 is what makes the case
 *       non-vacuous (the same shape as qa/slow-subscriber.cpp's staleEvicted() assertion).
 *   E4  teardown joins the encoder thread in bounded time, and the frames already admitted when it
 *       was called still go out, in order (stop drains; it does not truncate).
 *   E5  the #31 admission checks still fire — they belong to the COPY step now, and the copy is where
 *       the plane bytes are actually read. A claimed length above GetDirectBufferCapacity() is
 *       refused before the copy, by name, and nothing reaches the sender. (qa/jni-boundary.cpp is
 *       still the ASan proof for the same gate; this is the proof it moved rather than vanished.)
 *   E6  the copy preserves what the packer reads off the plane ADDRESSES, not just the bytes: the
 *     semi-planar shapes are one allocation described twice, and the malformed "both planes at the
 *     same address" pair must still be refused through the ring. A copy that put Cb and Cr into two
 *     separate buffers would turn every semi-planar device into SeparateStride2 and quietly un-decide
 *     that refusal; the byte output would not change, which is exactly why this case asserts the
 *     refusal and the layout name rather than the pixels alone.
 *
 * Build + run: bash qa/build-host.sh   (the et-* section, which also runs the mutants and the
 * pre-decoupling control against the OMTBridge.cpp pinned at ET_PREFIX_REV).
 */
#include "OMTSender.h"
#include "OMTProtocol.h"
#include "libomt.h"

#include <jni.h>            // qa/host-jni/jni.h — see the comment at the top of that file

#include "fake-image.h"
#include "nv12-oracle.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace omt;
using clock_ = std::chrono::steady_clock;

static int failures = 0;
static void ok(bool cond, const std::string& what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++failures;
}
static bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }
static long msSince(clock_::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_::now() - t0).count();
}

/* The bound on the callback. The encode behind it takes 70 ms, so a callback that still waits for
 * the codec is >= 70 ms here and the case is red with a 4x margin; a callback that only copies runs
 * in ~1 ms on this box under ASan, so the pass side has the same margin the other way. Both numbers
 * are printed with every run so a slow host is visible in the log rather than silently tolerated. */
static const long CALLBACK_BOUND_MS = 15;
/* How long nativeCleanup may take to stop and join the encoder thread. The stop drains at most
 * OMT_RING_DEPTH frames at 70 ms, so ~0.2 s is the real bound and this is the "it hung" bound. */
static const long TEARDOWN_BOUND_MS = 3000;

static const int FRAME_W = 1280;
static const int FRAME_H = 720;
static const int Y_ROW = 1280;
static const int CHROMA_ROW = 1280;      // semi-planar: a chroma row is as wide as a luma row
static const int BURST = 16;             // frames pushed back-to-back in E1..E3

namespace qa_stub { extern int encodeDelayUs; extern int payloadBytes; }

/* ------------------------------------------------------------------ *
 * The log — the bridge refuses by logging a reason, so the reason has to be readable.
 * ------------------------------------------------------------------ */
/* The log, behind its own mutex.
 *
 * It HAS to be locked now and did not have to be before: the bridge logs from whichever thread is
 * running the frame, and the whole point of this branch is that the frame path moved onto a second
 * thread. An unsynchronized std::string here is a realloc on the encoder thread while the producer
 * appends — a heap-use-after-free that ASan blames on this file, in a scenario whose subject is
 * something else entirely (it took 1 run in 5 to show up, which is the other half of why this file
 * is the one that must be deterministic). Every read below is a snapshot() taken under the same
 * lock, never a reference into the live string. */
struct QaLog {
    std::mutex m;
    std::string text;
    void append(const char* s) { std::lock_guard<std::mutex> g(m); text += s; text += '\n'; }
    size_t size() { std::lock_guard<std::mutex> g(m); return text.size(); }
    void clear() { std::lock_guard<std::mutex> g(m); text.clear(); }
    std::string from(size_t off) { std::lock_guard<std::mutex> g(m); return text.size() > off ? text.substr(off) : std::string(); }
    std::string snapshot() { std::lock_guard<std::mutex> g(m); return text; }
} g_qaLog;
extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
    (void)prio; (void)tag;
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_qaLog.append(buf);
    return 0;
}
/* The old name, so the assertions below read the way they always did. */
#define LOG_SNAPSHOT (g_qaLog.snapshot())

/* ------------------------------------------------------------------ *
 * Fake direct ByteBuffers — same discipline as qa/jni-boundary.cpp: the allocation is exactly the
 * capacity GetDirectBufferCapacity reports, so a read the boundary did not vet is an ASan report.
 * ------------------------------------------------------------------ */
struct FakeBuf {
    /* Two handles may share ONE allocation — that is what a semi-planar Image is: planes[1] and
     * planes[2] are the same buffer described twice, one byte apart. `shared` is that one allocation;
     * `own` is a plane that really is on its own. The packer decides the chroma layout from where the
     * two windows sit RELATIVE to each other, so a fixture that could not express the sharing would
     * have been testing a shape the HAL never reports. */
    std::shared_ptr<std::vector<uint8_t>> shared;
    std::vector<uint8_t> own;
    size_t offset = 0;
    size_t capacity = 0;
    const uint8_t* base() const { return shared ? shared->data() : own.data(); }
};
static std::vector<std::unique_ptr<FakeBuf>> g_bufs;

static jobject makeHandle(std::vector<uint8_t> backing, size_t offset, size_t capacity) {
    auto b = std::make_unique<FakeBuf>();
    b->own = std::move(backing);
    b->offset = offset;
    b->capacity = capacity;
    FakeBuf* raw = b.get();
    g_bufs.push_back(std::move(b));
    return reinterpret_cast<jobject>(raw);
}

/** Two handles over one allocation: the bytes are the HAL's, the offsets are the plane's. */
static jobject makeSharedHandle(std::shared_ptr<std::vector<uint8_t>> alloc, size_t offset, size_t capacity) {
    auto b = std::make_unique<FakeBuf>();
    b->shared = std::move(alloc);
    b->offset = offset;
    b->capacity = capacity;
    FakeBuf* raw = b.get();
    g_bufs.push_back(std::move(b));
    return reinterpret_cast<jobject>(raw);
}

void* JNIEnv::GetDirectBufferAddress(jobject buf) const {
    if (buf == nullptr) return nullptr;                      // a non-direct buffer, as JNI reports it
    FakeBuf* b = reinterpret_cast<FakeBuf*>(buf);
    const uint8_t* base = b->shared ? b->shared->data() : b->own.data();
    const size_t n = b->shared ? b->shared->size() : b->own.size();
    return (n == 0 || base == nullptr) ? nullptr : const_cast<uint8_t*>(base + b->offset);
}
jlong JNIEnv::GetDirectBufferCapacity(jobject buf) const {
    if (buf == nullptr) return -1;
    FakeBuf* b = reinterpret_cast<FakeBuf*>(buf);
    return (jlong)b->capacity;
}
const char* JNIEnv::GetStringUTFChars(jstring s, jboolean* isCopy) const {
    if (isCopy) *isCopy = JNI_FALSE;
    return reinterpret_cast<const char*>(s);
}
void JNIEnv::ReleaseStringUTFChars(jstring, const char*) const {}
jstring JNIEnv::NewStringUTF(const char* bytes) const { return reinterpret_cast<jstring>(const_cast<char*>(bytes)); }

/* ------------------------------------------------------------------ *
 * The C API the bridge calls, over a REAL omt::Sender with a REAL video-subscribed subscriber.
 *
 * omt_send is the seam the assertions read: every frame the encoder thread hands the sender is
 * recorded with the payload bytes it carried and the destination it was handed, in arrival order.
 * ------------------------------------------------------------------ */
static Sender* g_shimSender = nullptr;
static int g_shimPort = 45351;

struct Delivered {
    int index = -1;                 // which frame the test built this one
    void* data = nullptr;           // the destination buffer the frame was packed into
    int dataLength = 0;
    std::vector<uint8_t> bytes;     // the payload, copied at the moment of sending
    std::string stamp;              // log text produced while this frame was in flight
};
static std::deque<Delivered> g_delivered;
static std::mutex g_delMtx;
static int g_sendCalls = 0;
/* Written by the main thread between cases, read by the channel's worker thread inside the frame
 * callback — the same two-thread shape as the log above, so the same rule applies. */
static std::atomic<bool> g_record{false};   // record only while a case is running

extern "C" {
omt_send_t* omt_send_create(const char* name, OMTQuality quality) {
    SenderConfig cfg;
    cfg.name = name ? name : "qa-encode-pipeline";
    cfg.port = g_shimPort;
    cfg.quality = (quality == OMTQuality_Low) ? Quality::Low
                : (quality == OMTQuality_Medium) ? Quality::Medium
                : (quality == OMTQuality_High) ? Quality::High : Quality::Default;
    auto* s = new Sender(cfg);
    if (!s->start()) { delete s; return nullptr; }
    g_shimSender = s;
    return reinterpret_cast<omt_send_t*>(s);
}
void omt_send_destroy(omt_send_t* instance) {
    auto* s = reinterpret_cast<Sender*>(instance);
    if (s) { s->stop(); delete s; }
    if (g_shimSender == s) g_shimSender = nullptr;
}
int omt_send(omt_send_t* instance, OMTMediaFrame* frame) {
    if (!instance || !frame) return 0;
    if (frame->Type != OMTFrameType_Video) return 0;
    MediaFrame mf{};
    mf.data = frame->Data; mf.dataLength = frame->DataLength;
    mf.width = frame->Width; mf.height = frame->Height; mf.stride = frame->Stride;
    mf.codec = frame->Codec; mf.frameRateN = frame->FrameRateN; mf.frameRateD = frame->FrameRateD;
    mf.aspectRatio = frame->AspectRatio; mf.flags = frame->Flags;
    mf.colorSpace = frame->ColorSpace; mf.timestamp = frame->Timestamp;
    if (g_record) {
        Delivered d;
        d.data = frame->Data;
        d.dataLength = frame->DataLength;
        const size_t logBefore = g_qaLog.size();
        d.bytes.assign(static_cast<uint8_t*>(frame->Data),
                       static_cast<uint8_t*>(frame->Data) + (frame->DataLength > 0 ? frame->DataLength : 0));
        // Identify the frame by the per-frame tag the fixture stamped into its first luma row.
        if (!d.bytes.empty()) d.index = (int)d.bytes[0] - 0x40;
        d.stamp = g_qaLog.from(logBefore);
        std::lock_guard<std::mutex> lk(g_delMtx);
        g_delivered.push_back(std::move(d));
    }
    ++g_sendCalls;
    return reinterpret_cast<Sender*>(instance)->send(mf);
}
int omt_send_connections(omt_send_t* instance) {
    return instance ? reinterpret_cast<Sender*>(instance)->connectionCount() : 0;
}
int omt_send_getaddress(omt_send_t*, char*, int) { return 0; }
int64_t omt_send_get_frames_sent(omt_send_t* i) { return i ? reinterpret_cast<Sender*>(i)->getStats().totalFramesSent : 0; }
int64_t omt_send_get_frames_dropped(omt_send_t* i) { return i ? reinterpret_cast<Sender*>(i)->getStats().totalFramesDropped : 0; }
int64_t omt_send_get_recent_drops_and_reset(omt_send_t* i) { return i ? reinterpret_cast<Sender*>(i)->getRecentDropsAndReset() : 0; }
int64_t omt_send_get_bytes_sent(omt_send_t* i) { return i ? reinterpret_cast<Sender*>(i)->getStats().totalBytesSent : 0; }
void omt_send_set_info_str(omt_send_t*, const char*, const char*) {}
void omt_send_set_quality(omt_send_t*, OMTQuality) {}
int omt_send_get_profile(omt_send_t*) { return 0; }
} // extern "C"

/* The production entry points. The two the decoupling ADDED are declared weak, so the SAME test
 * binary links against the pre-decoupling OMTBridge.cpp that qa/build-host.sh extracts for
 * et-prefix: there they resolve to null, the test says so in its own output, and it measures the
 * callback through the entry point that tree actually had — which is the blocking one. A prefix
 * control that died at the linker would prove nothing about the gate it exists to prove. */
extern "C" {
jboolean Java_net_sourceforge_opencamera_OMTSender_nativeInit(JNIEnv*, jobject, jstring, jint, jint,
                                                               jint, jint, jint);
void Java_net_sourceforge_opencamera_OMTSender_nativeCleanup(JNIEnv*, jobject);
jboolean Java_net_sourceforge_opencamera_OMTSender_nativeSendFramePlanes(
        JNIEnv*, jobject, jobject dstBuffer,
        jobject yBuffer, jint ySize, jint yRowStride, jint yPixelStride,
        jobject uBuffer, jint uSize, jint uRowStride, jint uPixelStride,
        jobject vBuffer, jint vSize, jint vRowStride, jint vPixelStride,
        jint width, jint height);
__attribute__((weak)) jboolean Java_net_sourceforge_opencamera_OMTSender_nativeQueueFramePlanes(
        JNIEnv*, jobject,
        jobject yBuffer, jint ySize, jint yRowStride, jint yPixelStride,
        jobject uBuffer, jint uSize, jint uRowStride, jint uPixelStride,
        jobject vBuffer, jint vSize, jint vRowStride, jint vPixelStride,
        jint width, jint height);
__attribute__((weak)) jlong Java_net_sourceforge_opencamera_OMTSender_nativeGetRingDrops(JNIEnv*, jobject);
}

static bool haveQueueEntry() {
    return &Java_net_sourceforge_opencamera_OMTSender_nativeQueueFramePlanes != nullptr;
}
static long long ringDrops() {
    if (&Java_net_sourceforge_opencamera_OMTSender_nativeGetRingDrops == nullptr) return -1;
    return (long long)Java_net_sourceforge_opencamera_OMTSender_nativeGetRingDrops(nullptr, nullptr);
}

/* ------------------------------------------------------------------ *
 * One frame of a fake Camera2 Image, with a per-frame tag the assertions can recognise.
 * ------------------------------------------------------------------ */
struct Frame {
    fakehal::FakeImage img;
    int index = 0;
    explicit Frame(int i, fakehal::Shape shape, int yRow, int chromaRow)
        : img(FRAME_W, FRAME_H, yRow, chromaRow, shape), index(i) {
        /* Tag the first luma sample with the frame number. The oracle reads the planes, so the
         * expected bytes follow the tag automatically and the payload identifies its own frame. */
        img.y[0] = (uint8_t)(0x40 + i);
    }
};

struct QueueResult {
    jboolean verdict = JNI_FALSE;
    long ms = 0;
    std::string said;
};

/* Drive one frame through the entry point the camera callback uses. */
/* Drive one frame through the entry point the camera callback uses. Pass a claim other than HONEST
 * to make the bridge's caller lie about how much of a plane is readable — the #31 defect. */
static const int64_t HONEST = INT64_MIN;

static QueueResult driveQueue(const Frame& f, int64_t yClaim = HONEST, int64_t uClaim = HONEST,
                              int64_t vClaim = HONEST, bool sameUvHandle = false) {
    JNIEnv env;
    const int64_t yCap = (int64_t)f.img.yAllocSize();
    const int64_t uCap = f.img.uCapacity();
    const int64_t vCap = f.img.vCapacity();
    const int64_t ySize = yClaim != HONEST ? yClaim : yCap;
    const int64_t uSize = uClaim != HONEST ? uClaim : f.img.uLimit();
    const int64_t vSize = vClaim != HONEST ? vClaim : f.img.vLimit();

    jobject yH = makeHandle(std::vector<uint8_t>(f.img.y.begin(), f.img.y.begin() + yCap), 0, (size_t)yCap);
    jobject uH = nullptr, vH = nullptr;
    const bool semi = f.img.shape == fakehal::Shape::SemiPlanarUV
                   || f.img.shape == fakehal::Shape::SemiPlanarVU;
    if (semi) {
        /* ONE allocation, two views — the shape the contract actually permits and the only shape the
         * packer can tell CB_CR from CR_CB from "malformed" about. The allocation is exactly as long
         * as the region the HAL reports, so a read past either window is ASan's, not the test's. */
        const size_t allocLen = (size_t)std::max<int64_t>(uCap, vCap)
                              + (size_t)std::min(f.img.uPtr() - f.img.uv.data(),
                                                 f.img.vPtr() - f.img.uv.data());
        auto alloc = std::make_shared<std::vector<uint8_t>>(f.img.uv.begin(),
                                                            f.img.uv.begin() + allocLen);
        const size_t uoff = (size_t)(f.img.uPtr() - f.img.uv.data());
        const size_t voff = sameUvHandle ? uoff : (size_t)(f.img.vPtr() - f.img.uv.data());
        uH = makeSharedHandle(alloc, uoff, (size_t)uCap);
        vH = makeSharedHandle(alloc, voff, (size_t)(sameUvHandle ? uCap : vCap));
    } else {
        uH = makeHandle(std::vector<uint8_t>(f.img.uPtr(), f.img.uPtr() + uCap), 0, (size_t)uCap);
        vH = makeHandle(std::vector<uint8_t>(f.img.vPtr(), f.img.vPtr() + vCap), 0, (size_t)vCap);
    }

    QueueResult r;
    const size_t logBefore = g_qaLog.size();
    auto t0 = clock_::now();
    if (haveQueueEntry()) {
        r.verdict = Java_net_sourceforge_opencamera_OMTSender_nativeQueueFramePlanes(
                &env, nullptr,
                yH, (jint)ySize, (jint)f.img.yRow, 1,
                uH, (jint)uSize, (jint)f.img.chromaRow, (jint)f.img.chromaPix(),
                vH, (jint)vSize, (jint)f.img.chromaRow, (jint)f.img.chromaPix(),
                (jint)FRAME_W, (jint)FRAME_H);
    } else {
        /* Pre-decoupling tree: the camera path WAS nativeSendFramePlanes. Measure that, because it
         * is the callback the control exists to condemn. */
        const int64_t need = (int64_t)FRAME_H * f.img.yRow + (int64_t)(FRAME_H / 2) * f.img.yRow;
        jobject dH = makeHandle(std::vector<uint8_t>((size_t)need, 0), 0, (size_t)need);
        r.verdict = Java_net_sourceforge_opencamera_OMTSender_nativeSendFramePlanes(
                &env, nullptr, dH,
                yH, (jint)ySize, (jint)f.img.yRow, 1,
                uH, (jint)uSize, (jint)f.img.chromaRow, (jint)f.img.chromaPix(),
                vH, (jint)vSize, (jint)f.img.chromaRow, (jint)f.img.chromaPix(),
                (jint)FRAME_W, (jint)FRAME_H);
    }
    r.ms = msSince(t0);
    r.said = g_qaLog.from(logBefore);
    return r;
}

/* The expected packed NV12 for a frame, from its own plane descriptors. */
static oracle::Expected expectedBytes(const Frame& f) {
    return oracle::nv12(f.img.planeY(), f.img.planeU(), f.img.planeV(), FRAME_W, FRAME_H);
}

/* ------------------------------------------------------------------ *
 * A subscriber that reads, so the sender really encodes and really delivers.
 *
 * Sender::send() returns 1 without encoding anything when nobody is connected, and returns 0 when a
 * channel would not take the bytes — so every "delivered" claim below needs a live, video-subscribed,
 * DRAINING channel, and needs it established through the real protocol rather than asserted.
 * ------------------------------------------------------------------ */
struct Stream {
    int fd = -1;
    int port = 0;
    std::atomic<bool> running{false};
    std::thread drain;
    Sender* sender = nullptr;

    void drainLoop() {
        while (running.load()) {
            struct pollfd p{fd, POLLIN, 0};
            if (::poll(&p, 1, 20) > 0) {
                char tmp[65536];
                if (::recv(fd, tmp, sizeof(tmp), 0) <= 0) return;   // peer gone
            }
        }
    }

    bool start(const char* name, int p) {
        port = p;
        g_shimPort = p;
        g_qaLog.clear();
        JNIEnv env;
        if (Java_net_sourceforge_opencamera_OMTSender_nativeInit(
                &env, nullptr, reinterpret_cast<jstring>(const_cast<char*>(name)),
                FRAME_W, FRAME_H, 30, 1, (jint)OMTQuality_Default) != JNI_TRUE) {
            std::printf("  [FAIL] SETUP: nativeInit refused the sender (%s): %s\n", name, LOG_SNAPSHOT.c_str());
            return false;
        }
        sender = g_shimSender;
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (::connect(fd, (sockaddr*)&a, sizeof(a)) != 0) {
            std::printf("  [FAIL] SETUP: %s — the subscriber could not connect to port %d\n", name, port);
            return false;
        }
        for (int i = 0; i < 150 && sender->connectionCount() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (sender->connectionCount() != 1) {
            std::printf("  [FAIL] SETUP: %s — the sender registered %d channels, not one\n",
                        name, sender->connectionCount());
            return false;
        }
        const char* xml = MetadataConstants::CHANNEL_SUBSCRIBE_VIDEO;
        const size_t xlen = std::strlen(xml) + 1;
        std::vector<uint8_t> m(FrameHeader::SIZE + xlen, 0);
        FrameHeader h{};
        h.version = 1; h.frameType = (uint8_t)FrameType::Metadata;
        h.metadataLength = (uint16_t)xlen; h.dataLength = (int32_t)xlen;
        std::memcpy(m.data(), &h, sizeof(h));
        std::memcpy(m.data() + FrameHeader::SIZE, xml, xlen);
        if (::send(fd, m.data(), m.size(), MSG_NOSIGNAL) != (ssize_t)m.size()) {
            std::printf("  [FAIL] SETUP: %s — the subscribe control frame did not go out\n", name);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));   // let receiverLoop process it
        running = true;
        drain = std::thread([this] { drainLoop(); });
        return true;
    }

    /* Stop in the order the product must: the encoder thread is joined FIRST (nativeCleanup does
     * that, and the assertion on how long it took is E4), and only then does the viewer go away.
     * Closing the socket first would let a wedged channel look like a clean shutdown. */
    void stop(bool timed = false, long* outMs = nullptr) {
        JNIEnv env;
        auto t0 = clock_::now();
        Java_net_sourceforge_opencamera_OMTSender_nativeCleanup(&env, nullptr);
        if (outMs) *outMs = msSince(t0);
        (void)timed;
        running = false;
        if (drain.joinable()) drain.join();
        if (fd >= 0) { ::close(fd); fd = -1; }
        sender = nullptr;
    }
};

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);   // the failure mode here is a HANG; an empty log helps nobody
    std::printf("encode-pipeline: the REAL OMTBridge.cpp, with a codec that takes 70 ms\n");

    std::printf("      capture entry point: %s\n",
                haveQueueEntry() ? "nativeQueueFramePlanes (the decoupled bridge)"
                                 : "nativeSendFramePlanes — NO QUEUE ENTRY POINT IN THIS TREE");
    qa_stub::encodeDelayUs = 70 * 1000;   // the codec is slow on purpose; see the top of this file
    qa_stub::payloadBytes = 1024;         // keep the socket from becoming the thing under test

    int nextPort = 45351;
    std::unique_ptr<Stream> stream(new Stream());
    if (!stream->start("qa-encode-pipeline", nextPort++)) return 1;
    ok(stream->sender != nullptr && stream->sender->connectionCount() == 1,
       "SETUP: one video-subscribed, draining subscriber is attached");

    /* ════════════════════════════ E1..E4: the burst ══════════════════════════════════════════ */
    std::vector<Frame> burst;
    burst.reserve(BURST);
    for (int i = 0; i < BURST; ++i)
        burst.emplace_back(i, fakehal::Shape::SemiPlanarUV, Y_ROW, CHROMA_ROW);

    g_record = true;
    long worstCbMs = 0;
    long long totalCbUs = 0;
    int admitted = 0;
    for (const Frame& f : burst) {
        QueueResult r = driveQueue(f);
        if (r.verdict == JNI_TRUE) ++admitted;
        if (r.ms > worstCbMs) worstCbMs = r.ms;
        totalCbUs += r.ms * 1000;
        if (r.ms > CALLBACK_BOUND_MS)
            std::printf("      frame %d: callback took %ld ms  [%s]\n", f.index, r.ms,
                        haveQueueEntry() ? "the callback is doing the encode" : "pre-decoupling tree");
    }
    std::printf("      %d/%d frames admitted; slowest callback %ld ms, mean %.3f ms, encode is %d ms\n",
                admitted, BURST, worstCbMs, (double)totalCbUs / (1000.0 * BURST),
                qa_stub::encodeDelayUs / 1000);

    /* E1 — the callback. This is the decoupling, stated as a number. */
    ok(worstCbMs < CALLBACK_BOUND_MS,
       "E1 every callback returned within " + std::to_string(CALLBACK_BOUND_MS) + " ms while an "
       "encode takes 70 ms — the ImageReader callback does not block in pack+encode+send "
       "(slowest " + std::to_string(worstCbMs) + " ms)");

    /* E4 — teardown, timed from the producer thread while the encoder is still chewing the burst.
     * The stop must drain what was admitted and then JOIN; a join taken while g_senderMutex is held
     * deadlocks here, which is what et-mutant-join-under-sender-lock is scored on. */
    long stopMs = 0;
    stream->stop(false, &stopMs);
    g_record = false;
    std::printf("      nativeCleanup returned after %ld ms (bound %ld ms)\n", stopMs, TEARDOWN_BOUND_MS);
    ok(stopMs < TEARDOWN_BOUND_MS,
       "E4 stopStreaming/cleanup joined the encoder thread in bounded time (" + std::to_string(stopMs)
       + " ms, bound " + std::to_string(TEARDOWN_BOUND_MS) + " ms)");

    const int delivered = (int)g_delivered.size();
    const long long drops = ringDrops();
    std::printf("      delivered %d of %d admitted; ring drops reported %lld\n",
                delivered, admitted, drops);

    /* E2 — FIFO with distinct payloads, and every delivered payload is the frame it claims. */
    bool ordered = delivered > 0;
    bool distinctDsts = delivered > 0;
    bool faithful = delivered > 0;
    std::string whyNot;
    int prev = -1;
    for (int i = 0; i < delivered; ++i) {
        const Delivered& d = g_delivered[i];
        if (d.index <= prev) { ordered = false; if (whyNot.empty()) whyNot = "frame " + std::to_string(d.index) + " arrived after " + std::to_string(prev); }
        prev = d.index;
        for (int j = 0; j < i; ++j)
            if (g_delivered[j].data == d.data) { distinctDsts = false; whyNot = "frames "
                + std::to_string(g_delivered[j].index) + " and " + std::to_string(d.index)
                + " were packed into the SAME destination buffer"; }
        if (d.index < 0 || d.index >= (int)burst.size()) { faithful = false; whyNot = "an unrecognised frame tag"; continue; }
        oracle::Expected e = expectedBytes(burst[d.index]);
        if (e.harnessError) { faithful = false; whyNot = "FIXTURE ERROR: " + e.why; break; }
        if ((int)e.bytes.size() != d.dataLength) {
            faithful = false; whyNot = "frame " + std::to_string(d.index) + " declared "
                + std::to_string(d.dataLength) + " bytes, the geometry calls for " + std::to_string(e.bytes.size());
            break;
        }
        for (size_t b = 0; b < e.bytes.size(); ++b) {
            if (d.bytes[b] != e.bytes[b]) {
                faithful = false;
                whyNot = "frame " + std::to_string(d.index) + " byte " + std::to_string(b) + ": got 0x"
                       + std::to_string(d.bytes[b]) + " want 0x" + std::to_string(e.bytes[b]);
                break;
            }
        }
        if (!faithful) break;
    }
    ok(ordered && delivered > 0,
       "E2 the frames reached the sender in the order they were admitted (FIFO, " + std::to_string(delivered)
       + " delivered)" + (ordered ? std::string() : "   [" + whyNot + "]"));
    ok(distinctDsts,
       "E2b each delivered frame was packed into its OWN destination buffer, not one shared frameBuffer"
       + (distinctDsts ? std::string() : "   [" + whyNot + "]"));
    ok(faithful,
       "E2c every delivered payload is byte-identical to the packer's oracle for THAT frame — the ring "
       "moved the bytes, it did not change them" + (faithful ? std::string() : "   [" + whyNot + "]"));

    /* E3 — the drop-oldest counter. */
    ok(admitted > delivered,
       "E3 the producer outran the encoder: " + std::to_string(admitted) + " admitted, "
       + std::to_string(delivered) + " delivered");
    ok(drops >= 1,
       "E3b the eviction counter moved: the excess was dropped, not accumulated ("
       + std::to_string(drops) + " reported)");
    ok(drops == (long long)(admitted - delivered),
       "E3c nothing went missing quietly: admitted == delivered + dropped (" + std::to_string(admitted)
       + " == " + std::to_string(delivered) + " + " + std::to_string(drops) + ")");
    {
        /* The policy is drop-OLDEST: when the ring cannot hold everything, what survives is the
         * newest frame, because that is the one a viewer is still watching. Requiring only the LAST
         * frame is not enough — a ring that recycles the newest slot in place (the mutant below)
         * keeps the last frame too, and delivers a picture that is otherwise BURST-deep behind the
         * camera. The last TWO admitted frames are each other's control: the counts and the order
         * cannot tell them apart, only which frames came out can. (Same invariant, one level up, as
         * qa/slow-subscriber.cpp's "the newest frame is always admitted" + staleEvicted() pair.) */
        std::vector<int> got;
        for (const Delivered& d : g_delivered) got.push_back(d.index);
        auto delivered = [&](int idx) { return std::find(got.begin(), got.end(), idx) != got.end(); };
        std::string list;
        for (int i : got) { if (!list.empty()) list += ","; list += std::to_string(i); }
        ok(delivered(BURST - 1) && delivered(BURST - 2),
           "E3d the last two frames admitted are both among those delivered — the ring evicts the "
           "oldest, so the newest frames always win slots (delivered indices: "
           + (list.empty() ? std::string("none") : list) + ")");
    }

    /* ════════════════════════ E5/E6: a fresh sender, one frame at a time ══════════════════════ */
    qa_stub::encodeDelayUs = 0;   // the cases below are about geometry, not about timing
    auto restart = [&](const char* name) -> bool {
        g_delivered.clear();
        if (stream) stream->stop();
        stream.reset(new Stream());
        return stream->start(name, nextPort++);
    };

    /* E5 — the #31 admission checks live on the copy step now. */
    if (restart("qa-e5")) {
        Frame f(0, fakehal::Shape::Planar, Y_ROW, 640);
        const int64_t yOk = (int64_t)f.img.yAllocSize(), uOk = f.img.uLimit(), vOk = f.img.vLimit();
        struct Over { const char* name; int64_t y, u, v; const char* needle; };
        const Over overs[] = {
            {"E5a ySize one byte past the Y plane's capacity", yOk + 1, uOk,     vOk,     "Y plane"},
            {"E5b uSize one byte past the Cb plane's capacity", yOk,    uOk + 1, vOk,     "Cb plane"},
            {"E5c vSize one byte past the Cr plane's capacity", yOk,    uOk,     vOk + 1, "Cr plane"},
            {"E5d a negative ySize",                            -1,      uOk,     vOk,     "Y plane"},
        };
        for (const auto& o : overs) {
            const int before = g_sendCalls;
            QueueResult r = driveQueue(f, o.y, o.u, o.v);
            ok(r.verdict == JNI_FALSE,
               std::string(o.name) + ": refused at the copy step, not queued"
               + (r.verdict == JNI_FALSE ? std::string() : std::string("   [ACCEPTED — the claim was trusted]")));
            ok(g_sendCalls == before, std::string(o.name) + ": and nothing reached the sender");
            ok(has(r.said, o.needle) && has(r.said, "capacity"),
               std::string(o.name) + ": the refusal names the buffer and says capacity"
               + "   [log: " + r.said + "]");
        }
        QueueResult honest = driveQueue(f);
        ok(honest.verdict == JNI_TRUE,
           "E5e the honest geometry is still admitted after every refusal — the gate is selective"
           + (honest.verdict == JNI_TRUE ? std::string() : "   [" + honest.said + "]"));
    }

    /* E6 — the copy preserves the address relationship the packer decides from. */
    {
        struct ShapeCase { const char* name; fakehal::Shape shape; int chromaRow; };
        const ShapeCase shapes[] = {
            // VU first: its layout NAME is asserted below, and it is the shape a separate copy of
            // each chroma window would silently re-decide as SeparateStride2.
            {"E6a semi-planar VU (one allocation, Cr first)", fakehal::Shape::SemiPlanarVU, CHROMA_ROW},
            {"E6b semi-planar NV12 (one allocation, Cb first)", fakehal::Shape::SemiPlanarUV, CHROMA_ROW},
            {"E6c separate stride-2 Cb/Cr allocations", fakehal::Shape::SeparateStride2, CHROMA_ROW},
            {"E6d planar I420 (the emulator's layout)", fakehal::Shape::Planar, 640},
        };
        for (const auto& s : shapes) {
            if (!restart("qa-e6")) break;
            g_record = true;
            Frame f(3, s.shape, Y_ROW, s.chromaRow);
            QueueResult r = driveQueue(f);
            stream->stop();
            g_record = false;
            ok(r.verdict == JNI_TRUE, std::string(s.name) + ": admitted by the ring"
               + (r.verdict == JNI_TRUE ? std::string() : std::string("   [refused: ") + r.said + "]"));
            bool bytes_ok = g_delivered.size() == 1;
            std::string detail = bytes_ok ? std::string("nothing was delivered") : std::string();
            if (bytes_ok) {
                const Delivered& d = g_delivered.front();
                oracle::Expected e = expectedBytes(f);
                if (e.harnessError) { bytes_ok = false; detail = "FIXTURE ERROR: " + e.why; }
                else if (d.index != f.index) { bytes_ok = false; detail = "the payload carries no frame tag"; }
                else if ((int)e.bytes.size() != d.dataLength) {
                    bytes_ok = false; detail = "declared " + std::to_string(d.dataLength) + " bytes, geometry wants "
                        + std::to_string(e.bytes.size());
                } else {
                    for (size_t b = 0; b < e.bytes.size(); ++b) {
                        if (d.bytes[b] != e.bytes[b]) {
                            bytes_ok = false;
                            detail = "first wrong byte at " + std::to_string(b);
                            break;
                        }
                    }
                }
            }
            ok(bytes_ok, std::string(s.name) + ": the delivered NV12 is byte-exact against the oracle"
               + (bytes_ok ? std::string() : "   [" + detail + "]"));
            g_delivered.clear();
        }

        /* E6e: the malformed pair — Cb and Cr at the SAME address with pixelStride 2. planChroma()
         * refuses it by name, and the only reason it still can is that the copy kept the two windows
         * in one allocation at their reported offsets. Copy them apart and this becomes a frame the
         * bridge accepts, which is a deleted gate wearing a refactor's clothes. */
        if (restart("qa-e6e")) {
            g_record = true;
            Frame f(7, fakehal::Shape::SemiPlanarUV, Y_ROW, CHROMA_ROW);
            QueueResult r = driveQueue(f, HONEST, HONEST, HONEST, /*sameUvHandle=*/true);
            stream->stop();
            g_record = false;
            (void)r;
            /* The copy is safe — it only ever reads the windows the admission let through — so the
             * frame is admitted to the ring and refused where the geometry is checked, on the
             * encoder thread, before a single byte of it reaches the sender. Both halves are
             * asserted: the named refusal, and the absence of a delivery. */
            ok(has(LOG_SNAPSHOT, "same base address"),
               "E6e Cb and Cr handed over at one address at pixelStride 2 are still refused by name "
               "through the ring — the copy did not flatten the geometry the packer decides from"
               "   [log: " + LOG_SNAPSHOT + "]");
            ok(g_delivered.empty(), "E6e and that frame never reached the sender");
            g_delivered.clear();
        }

        /* E6f: the layout the ring reports is the layout the HAL described, not the layout a
         * flattened copy would invent. */
        if (restart("qa-e6f")) {
            g_record = true;
            Frame f(9, fakehal::Shape::SemiPlanarVU, Y_ROW, CHROMA_ROW);
            driveQueue(f);
            stream->stop();
            g_record = false;
            ok(has(LOG_SNAPSHOT, "semiplanar VU / CR_CB"),
               "E6f the ring logged the CR_CB layout the descriptors describe"
               "   [log: " + LOG_SNAPSHOT + "]");
            g_delivered.clear();
        }
    }

    if (stream) stream->stop();

    std::printf("\nencode-pipeline: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
