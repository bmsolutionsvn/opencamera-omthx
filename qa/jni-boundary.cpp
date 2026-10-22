/*
 * jni-boundary.cpp — the JNI entry point may only read what the buffer it was handed really holds.
 *
 * WHAT IS UNDER TEST, AND WHY IT IS THE REAL THING
 * app/src/main/cpp/OMTBridge.cpp is compiled into this binary — the production translation unit, not
 * a transcription — against qa/host-jni/jni.h, and the test calls its
 * Java_net_sourceforge_opencamera_OMTSender_nativeSendFramePlanes() directly. Behind it sits the real
 * omtpack::packNv12(), the real omt::Sender and the real VMX codec; the only substituted part of the
 * call chain is the JNIEnv, which is a struct the test owns. (qa/build-host.sh links the real
 * libomt/cpp and libvmx/src for this binary; the omt_* wrappers below are the same thin shims
 * libomt/cpp/libomt_android.cpp provides, re-declared only so the scenario can pick its own port —
 * that file pins Constants::NETWORK_PORT_START=6400 — and can record the MediaFrame the bridge handed
 * the sender, which is the geometry the wire would carry.)
 *
 * THE DEFECT THIS MEASURES (parent-verified: app/src/main/cpp/OMTBridge.cpp:225-236). The destination
 * capacity is checked — GetDirectBufferCapacity(dstBuffer) reaches the packer — and all four
 * addresses are null-checked. But ySize/uSize/vSize, which arrive as plain ints from Java, go
 * straight into omtpack::Plane{} with no comparison against GetDirectBufferCapacity() of the buffer
 * each one came from. The packer's own bounds checks are all of the form `plane.size < need`: they
 * trust `size`. Overstate a size and every one of them is defeated, so the packer happily walks off
 * the end of the plane allocation. On the phone that is a read past the ImageReader allocation; here,
 * where each plane is malloc'd to EXACTLY the capacity the bridge is told about, it is an ASan
 * heap-buffer-overflow — which is the point: the report, not an assertion, is the proof the read was
 * real. qa/build-host.sh's jb-mutant-no-capacity-admission deletes the admission and this same binary
 * dies under ASan; the fixed build refuses before the first read and names the buffer.
 *
 * WHY THE FIXTURES ARE ALLOCATED TO EXACTLY THEIR CAPACITY. Slack would turn an out-of-bounds read
 * into a silent read of the same allocation, and the test would pass while the defect shipped. Same
 * discipline as qa/planar-chroma.cpp's planes and qa/build-host.sh's mp3 mutant.
 *
 * Android's own words on what a plane buffer is and is not:
 *   https://developer.android.com/reference/android/graphics/ImageFormat#YUV_420_888
 *   https://developer.android.com/reference/android/media/Image.Plane#getBuffer()
 *
 * Build + run: bash qa/build-host.sh   (the jb-* section; it also runs the admission-removed mutant)
 */
#include "OMTSender.h"
#include "OMTProtocol.h"
#include "libomt.h"

#include <jni.h>            // qa/host-jni/jni.h — see the comment at the top of that file

#include "fake-image.h"
#include "nv12-oracle.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
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
static std::string hexb(uint8_t b) { char t[8]; snprintf(t, sizeof t, "%02X", b); return std::string(t); }

/* ------------------------------------------------------------------ *
 * The log. OMTBridge.cpp refuses a frame by logging a reason, and "refused with a named reason" is
 * only checkable if the reason is captured. Every line lands in g_log; each case looks at what the
 * bridge said *while it ran* (drive() snapshots the length first).
 * ------------------------------------------------------------------ */
static std::string g_log;
extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
    (void)prio; (void)tag;
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log += buf;
    g_log += '\n';
    return 0;
}

/* ------------------------------------------------------------------ *
 * Fake direct ByteBuffers.
 *
 * A handle is a FakeBuf. The allocation is malloc'd to exactly `capacity` bytes — the number
 * GetDirectBufferCapacity reports — so ASan's redzone begins at the byte the boundary must refuse to
 * read. `offset` is how a semi-planar Image hands planes[1] and planes[2] as two views of ONE
 * allocation, which is the shape the packer must not misread.
 * ------------------------------------------------------------------ */
struct FakeBuf {
    std::vector<uint8_t> bytes;
    size_t offset = 0;
    size_t capacity = 0;
};
static std::vector<std::unique_ptr<FakeBuf>> g_bufs;

static jobject makeHandle(std::vector<uint8_t> backing, size_t offset, size_t capacity) {
    auto b = std::make_unique<FakeBuf>();
    b->bytes = std::move(backing);
    b->offset = offset;
    b->capacity = capacity;
    FakeBuf* raw = b.get();
    g_bufs.push_back(std::move(b));
    return reinterpret_cast<jobject>(raw);
}

void* JNIEnv::GetDirectBufferAddress(jobject buf) const {
    if (buf == nullptr) return nullptr;                      // a non-direct buffer, as JNI reports it
    FakeBuf* b = reinterpret_cast<FakeBuf*>(buf);
    return b->bytes.empty() ? nullptr : b->bytes.data() + b->offset;
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
 * The C API the bridge calls, over a REAL omt::Sender (see the note at the top of the file).
 * ------------------------------------------------------------------ */
static Sender* g_shimSender = nullptr;
static MediaFrame g_lastFrame{};
static int g_sendCalls = 0;
static int g_shimPort = 45342;

extern "C" {
omt_send_t* omt_send_create(const char* name, OMTQuality quality) {
    SenderConfig cfg;
    cfg.name = name ? name : "qa-jni-boundary";
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
    g_lastFrame = mf; ++g_sendCalls;
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

/* The production entry points, as OMTBridge.cpp defines them. */
extern "C" {
jboolean Java_net_sourceforge_opencamera_OMTSender_nativeInit(JNIEnv*, jobject, jstring, jint, jint,
                                                               jint, jint, jint);
void Java_net_sourceforge_opencamera_OMTSender_nativeCleanup(JNIEnv*, jobject);
jboolean Java_net_sourceforge_opencamera_OMTSender_nativeSendFramePlanes(
        JNIEnv*, jobject,
        jobject dstBuffer,
        jobject yBuffer, jint ySize, jint yRowStride, jint yPixelStride,
        jobject uBuffer, jint uSize, jint uRowStride, jint uPixelStride,
        jobject vBuffer, jint vSize, jint vRowStride, jint vPixelStride,
        jint width, jint height);
jboolean Java_net_sourceforge_opencamera_OMTSender_nativeSendCoded(
        JNIEnv*, jobject, jobject buffer, jint offset, jint length, jint codec,
        jint width, jint height, jint frameRateN, jint frameRateD, jlong ptsNs);
}

/* ------------------------------------------------------------------ *
 * Drive one frame through the production JNI entry point.
 *
 * `yClaim`/`uClaim`/`vClaim` are what the caller SAYS is readable; the capacity each buffer really
 * has is always the fixture's own region. A case that passes a claim above the capacity is the
 * defect; the boundary's job is to refuse it before the first read.
 * ------------------------------------------------------------------ */
struct Drive {
    jboolean verdict = JNI_FALSE;
    std::string said;            // log lines the bridge produced for THIS call
    std::vector<uint8_t> dst;    // the packed buffer, so the caller can say what came out
    int stride = 0;
    int dataLength = 0;
    int sends = 0;               // did omt_send run at all — i.e. did packing get as far as the sender
};

/** The capacities to register for the three plane buffers. Empty = the honest capacity the fixture
 *  reports. A capacity BELOW what the geometry requires is the situation the packer cannot see: its
 *  checks are `claimed >= need`, so a claim that is big enough hides a buffer that is too small, and
 *  only GetDirectBufferCapacity() tells the truth. */
struct Caps { int64_t y = -1, u = -1, v = -1; };

static Drive drive(const fakehal::FakeImage& img, int64_t yClaim, int64_t uClaim, int64_t vClaim,
                   int w, int h, long dstSlop = 0, const Caps& caps = Caps()) {
    JNIEnv env;
    Drive d;
    const int64_t need = (int64_t)h * img.yRow + (int64_t)(h / 2) * img.yRow + dstSlop;
    d.dst.assign((size_t)std::max<int64_t>(need, 1), 0xA5);   // 0xA5: never a plausible pixel value

    /* Each plane is its OWN allocation of exactly the capacity the bridge will be told about, holding
     * exactly the bytes the HAL would have left there — including the odd half of a stride-2 buffer
     * and the 0xFF row padding, which is what makes a wrong copy visible. Shrinking a capacity below
     * what the geometry requires is how a scenario asks whether anything checks the claim at all:
     * ASan's redzone is the answer. */
    const int64_t yCap = caps.y > 0 ? caps.y : (int64_t)img.yAllocSize();
    const int64_t uCap = caps.u > 0 ? caps.u : img.uCapacity();
    const int64_t vCap = caps.v > 0 ? caps.v : img.vCapacity();
    jobject yH = makeHandle(std::vector<uint8_t>(img.y.begin(), img.y.begin() + yCap), 0, (size_t)yCap);
    jobject uH = makeHandle(std::vector<uint8_t>(img.uPtr(), img.uPtr() + uCap), 0, (size_t)uCap);
    jobject vH = makeHandle(std::vector<uint8_t>(img.vPtr(), img.vPtr() + vCap), 0, (size_t)vCap);
    jobject dH = makeHandle(std::vector<uint8_t>(d.dst.begin(), d.dst.end()), 0, d.dst.size());

    const size_t logBefore = g_log.size();
    const int sendsBefore = g_sendCalls;
    d.verdict = Java_net_sourceforge_opencamera_OMTSender_nativeSendFramePlanes(
            &env, nullptr, dH,
            yH, (jint)yClaim, (jint)img.yRow, 1,
            uH, (jint)uClaim, (jint)img.chromaRow, (jint)img.chromaPix(),
            vH, (jint)vClaim, (jint)img.chromaRow, (jint)img.chromaPix(),
            (jint)w, (jint)h);
    d.said = g_log.substr(logBefore);
    d.sends = g_sendCalls - sendsBefore;
    /* Read the destination back out of the fake buffer: the bridge wrote it by address, exactly as
     * the JVM would, and the caller is the only party that can say what actually came out. */
    memcpy(d.dst.data(), reinterpret_cast<FakeBuf*>(dH)->bytes.data(), d.dst.size());
    d.stride = g_lastFrame.stride;
    d.dataLength = g_lastFrame.dataLength;
    return d;
}

/* The oracle, applied to the bytes the bridge produced. 0 means every byte matched. */
static long long mismatchesAgainst(const Drive& d, const fakehal::FakeImage& img, int w, int h,
                                   std::string& detail) {
    oracle::Expected e = oracle::nv12(img.planeY(), img.planeU(), img.planeV(), w, h);
    if (e.harnessError) { detail = "FIXTURE ERROR: " + e.why; return -1; }
    if (d.sends == 0) { detail = "the bridge refused the frame, so there are no packed bytes to compare"; return -1; }
    if (d.dataLength != (int)e.dataLength) {
        detail = "the sender was handed dataLength " + std::to_string(d.dataLength)
               + " but the geometry calls for " + std::to_string(e.dataLength);
        return -1;
    }
    long long bad = 0;
    for (size_t i = 0; i < e.bytes.size(); ++i) {
        if (d.dst[i] != e.bytes[i]) {
            if (!bad) detail = "first wrong byte at " + std::to_string(i) + ": got 0x" + hexb(d.dst[i])
                             + " want 0x" + hexb(e.bytes[i]);
            ++bad;
        }
    }
    if (!bad) {   // nothing outside the declared length may be touched either
        for (size_t i = (size_t)e.dataLength; i < d.dst.size(); ++i) {
            if (d.dst[i] != 0xA5) { detail = "wrote past the declared length at " + std::to_string(i); ++bad; break; }
        }
    }
    return bad;
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::printf("jni-boundary: the REAL app/src/main/cpp/OMTBridge.cpp, driven as the JVM would\n");

    JNIEnv env;
    if (Java_net_sourceforge_opencamera_OMTSender_nativeInit(
            &env, nullptr, reinterpret_cast<jstring>(const_cast<char*>("qa-jni-boundary")),
            256, 64, 30, 1, (jint)OMTQuality_Default) != JNI_TRUE) {
        std::printf("  [FAIL] nativeInit refused the sender on port %d — cannot score anything: %s\n",
                    g_shimPort, g_log.c_str());
        return 1;
    }
    Sender* sender = g_shimSender;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons(g_shimPort);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    ok(::connect(fd, (sockaddr*)&a, sizeof(a)) == 0, "a subscriber socket connected to the sender");
    for (int i = 0; i < 100 && sender->connectionCount() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ok(sender->connectionCount() == 1, "the sender accepted it as a channel");
    {   // Without the subscribe control frame Sender::send skips the channel and nothing encodes.
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

    /* ---------------- A: the legal shapes, through the production entry point ----------------
     * Asserted against the oracle byte-for-byte, so this section also proves defect (1) was reachable
     * from Java and not only from qa/planar-chroma.cpp's direct calls into the header. */
    struct ShapeCase { const char* name; fakehal::Shape shape; int yRow; int chromaRow; };
    const ShapeCase shapes[] = {
        {"A1 semi-planar NV12 (one UV allocation, Cb first)", fakehal::Shape::SemiPlanarUV,    256, 256},
        {"A2 semi-planar NV12, padded luma",                  fakehal::Shape::SemiPlanarUV,    320, 320},
        {"A3 semi-planar VU (one UV allocation, Cr first)",   fakehal::Shape::SemiPlanarVU,    256, 256},
        {"A4 separate stride-2 Cb/Cr allocations",            fakehal::Shape::SeparateStride2, 256, 256},
        {"A5 planar I420 (the emulator's layout)",            fakehal::Shape::Planar,          256, 128},
        {"A6 planar I420, padded rows",                       fakehal::Shape::Planar,          320, 160},
    };
    for (const auto& s : shapes) {
        fakehal::FakeImage img(256, 64, s.yRow, s.chromaRow, s.shape);
        Drive d = drive(img, (int64_t)img.yAllocSize(), img.uLimit(), img.vLimit(), 256, 64);
        std::string detail;
        long long bad = mismatchesAgainst(d, img, 256, 64, detail);
        ok(d.verdict == JNI_TRUE, std::string(s.name) + ": the bridge accepted it"
                                 + (d.verdict == JNI_TRUE ? std::string()
                                     : std::string("   [refused: ") + d.said + "]"));
        ok(bad == 0, std::string(s.name) + ": every packed byte matches the oracle built from the "
                    "plane descriptors alone" + (bad == 0 ? std::string() : std::string("   [") + detail + "]"));
        ok(d.sends == 1, std::string(s.name) + ": and the frame reached omt_send exactly once");
    }

    /* ---------------- B: caller-supplied lengths the buffer cannot honour ----------------
     * These are the defect. On the unfixed bridge they are not refusals at all — the packer trusts the
     * claim, walks off the plane allocation, and ASan reports the read. */
    {
        fakehal::FakeImage img(256, 64, 256, 128, fakehal::Shape::Planar);
        const int64_t yOk = (int64_t)img.yAllocSize(), uOk = img.uLimit(), vOk = img.vLimit();

        struct Overcase { const char* name; int64_t y, u, v; const char* needle; };
        const Overcase overs[] = {
            /* +1 is the smallest possible lie: it cannot be excused as rounding, and the packer's own
             * checks are all `size < need`, so a bigger claim always slips past every one of them. */
            {"B1 ySize one byte past the Y plane's capacity",        yOk + 1, uOk,     vOk,     "Y plane"},
            {"B2 uSize one byte past the Cb plane's capacity",       yOk,     uOk + 1, vOk,     "Cb plane"},
            {"B3 vSize one byte past the Cr plane's capacity",       yOk,     uOk,     vOk + 1, "Cr plane"},
            {"B4 ySize claimed as a whole megabyte",                 1 << 20, uOk,     vOk,     "Y plane"},
            {"B5 uSize claimed as a whole megabyte",                 yOk,     1 << 20, vOk,     "Cb plane"},
            {"B6 vSize claimed as a whole megabyte",                 yOk,     uOk,     1 << 20, "Cr plane"},
            {"B7 a negative ySize",                                  -1,      uOk,     vOk,     "Y plane"},
            {"B8 a negative uSize",                                  yOk,     -7,      vOk,     "Cb plane"},
        };
        for (const auto& o : overs) {
            Drive d = drive(img, o.y, o.u, o.v, 256, 64);
            ok(d.verdict == JNI_FALSE, std::string(o.name) + ": refused, not packed"
                                       + (d.verdict == JNI_FALSE ? std::string()
                                           : std::string("   [ACCEPTED — the claim was trusted]")));
            ok(d.sends == 0, std::string(o.name) + ": nothing reached the sender"
                            + (d.sends == 0 ? std::string() : "   [it was sent — the read already happened]"));
            ok(has(d.said, o.needle) && has(d.said, "capacity"),
               std::string(o.name) + ": the refusal names the buffer and says capacity"
                                       "   [log: " + d.said + "]");
        }

        /* ---- B'-*: the same lies, but shaped so they actually reach memory.
         *
         * Overstating a length is only dangerous when the plane's REAL capacity is below what the
         * geometry requires — the packer's checks are `claimed >= need`, so a claim that clears `need`
         * hides a buffer that does not. Here the claim is exactly what a truthful caller would report
         * (the number the geometry demands) and the allocation is one byte smaller, which is precisely
         * what Image.Plane.getBuffer() can report and what GetDirectBufferCapacity() would contradict.
         * Nothing but the capacity check can stop it: on the unfixed bridge these four reads walk past
         * the end of the plane and ASan says so — qa/build-host.sh's jb-mutant-no-capacity-admission
         * is this same binary, and it dies with a heap-buffer-overflow READ inside packNv12. */
        struct Oobcase { const char* name; fakehal::Shape shape; int chromaRow; int64_t capY, capU, capV;
                         int64_t claimY, claimU, claimV; const char* needle; };
        const Oobcase oobs[] = {
            /* 256x64, y rowStride 256: the luma plane needs (64-1)*256+256 = 16384 bytes. */
            {"B9  a Y plane one byte shorter than height*rowStride, claimed as full",
             fakehal::Shape::Planar, 128, 16384 - 1, -1, -1, 16384, 4096, 4096, "Y plane"},
            /* planar Cb: (32-1)*128+128 = 4096. */
            {"B10 a Cb plane one byte shorter than its chroma rows, claimed as full",
             fakehal::Shape::Planar, 128, -1, 4096 - 1, -1, 16384, 4096, 4096, "Cb plane"},
            {"B11 a Cr plane one byte shorter than its chroma rows, claimed as full",
             fakehal::Shape::Planar, 128, -1, -1, 4096 - 1, 16384, 4096, 4096, "Cr plane"},
            /* semi-planar: the verbatim copy reads (32-1)*256+256 = 8192 bytes of the shared UV plane. */
            {"B12 a shared UV plane one byte short, claimed as full",
             fakehal::Shape::SemiPlanarUV, 256, -1, 8192 - 1, 8192 - 1, 16384, 8192, 8192, "Cb plane"},
        };
        for (const auto& o : oobs) {
            fakehal::FakeImage img(256, 64, 256, o.chromaRow, o.shape);
            Drive d = drive(img, o.claimY, o.claimU, o.claimV, 256, 64, 0,
                            Caps{o.capY, o.capU, o.capV});
            ok(d.verdict == JNI_FALSE, std::string(o.name) + ": refused before the first read"
                                       + (d.verdict == JNI_FALSE ? std::string()
                                           : std::string("   [ACCEPTED — the packer read past the buffer]")));
            ok(d.sends == 0, std::string(o.name) + ": nothing reached the sender"
                            + (d.sends == 0 ? std::string() : "   [it was sent — the read already happened]"));
            ok(has(d.said, o.needle) && has(d.said, "capacity"),
               std::string(o.name) + ": the refusal names the buffer and says capacity"
                                       "   [log: " + d.said + "]");
        }

        /* B13: the selectivity control. The gate is `claim > capacity`, so a claim EQUAL to the
         * capacity — which is exactly what a truthful Image.Plane reports — must still pack. Without
         * this, "refuse everything" would satisfy the twelve cases above. */
        Drive exact = drive(img, (int64_t)img.yAllocSize(), img.uCapacity(), img.vCapacity(), 256, 64);
        std::string detail;
        ok(exact.verdict == JNI_TRUE && mismatchesAgainst(exact, img, 256, 64, detail) == 0,
           "B13 a claim exactly equal to each capacity is accepted and byte-exact — the gate is "
           "selective, not a blanket refusal" + (detail.empty() ? std::string() : "   [" + detail + "]"));

        /* B14: and the honest frame still packs after every refusal above. */
        Drive again = drive(img, yOk, uOk, vOk, 256, 64);
        ok(again.verdict == JNI_TRUE, "B14 the same honest geometry packs again — the refusals above "
                                      "did not wedge the bridge"
                                      + (again.verdict == JNI_TRUE ? std::string() : "   [" + again.said + "]"));
    }

    /* ---------------- C: the checks that were already there must stay ---------------- */
    {
        fakehal::FakeImage img(256, 64, 256, 128, fakehal::Shape::Planar);
        Drive d = drive(img, (int64_t)img.yAllocSize(), img.uLimit(), img.vLimit(), 256, 64, /*dstSlop=*/-1);
        ok(d.verdict == JNI_FALSE && has(d.said, "destination buffer smaller"),
           "C1 a destination one byte short of stride*height*3/2 is still refused by the packer's own "
           "gate   [log: " + d.said + "]");

        JNIEnv env2;
        const int64_t need = 64 * 256 + 32 * 256;
        jobject dH = makeHandle(std::vector<uint8_t>((size_t)need, 0), 0, (size_t)need);
        jobject yH = makeHandle(std::vector<uint8_t>(img.y.size(), 0), 0, img.y.size());
        jobject uH = makeHandle(std::vector<uint8_t>(4096, 0), 0, 4096);
        const size_t logBefore = g_log.size();
        jboolean v = Java_net_sourceforge_opencamera_OMTSender_nativeSendFramePlanes(
                &env2, nullptr, dH, yH, (jint)img.y.size(), 256, 1, uH, 4096, 128, 1,
                nullptr /* vBuffer: not a direct buffer at all */, 4096, 128, 1, 256, 64);
        std::string said = g_log.substr(logBefore);
        ok(v == JNI_FALSE && has(said, "not a direct ByteBuffer"),
           "C2 a null / non-direct plane is still refused with the existing named reason"
           "   [log: " + said + "]");
    }

    /* ---------------- D: the wire, not the verdict ----------------
     * A JNI_TRUE that never produced bytes on the socket would be the same lie the app told before
     * this suite existed. Drain the subscriber and require real video. */
    {
        fakehal::FakeImage img(256, 64, 256, 128, fakehal::Shape::Planar);
        timeval tv{2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        const long long before = (long long)sender->getStats().totalBytesSent;
        Drive d = drive(img, (int64_t)img.yAllocSize(), img.uLimit(), img.vLimit(), 256, 64);
        ok(d.verdict == JNI_TRUE, "D1 the honest frame was accepted by the bridge");
        uint8_t sink[65536];
        ssize_t got = 0, total = 0;
        for (int i = 0; i < 8; ++i) { got = ::recv(fd, sink, sizeof(sink), 0); if (got <= 0) break; total += got; }
        ok(total > 0, "D2 and bytes actually crossed the subscriber socket (" + std::to_string(total)
                      + " bytes received; sender total " + std::to_string(before) + " -> "
                      + std::to_string(sender->getStats().totalBytesSent) + ")");
        ok(sender->encodeFailures() == 0, "D3 with no encode failures, so the counter keeps meaning what it says"
                                          "   [failures=" + std::to_string(sender->encodeFailures()) + "]");
    }

    /* ---------------- C: the coded (OMT-Hx) entry point ----------------
     * Java posts one already-encoded access unit per frame. The bridge must hand the sender exactly
     * that buffer with the caller's geometry, and turn the nanosecond PTS into the 100 ns ticks the
     * OMT header carries - a microsecond timestamp makes every frame look 10x too close, which no
     * same-process test can notice unless it asks for the number. */
    {
        std::vector<uint8_t> au(2048);
        for (size_t i = 0; i < au.size(); ++i) au[i] = (uint8_t)(i * 13 + 5);
        jobject handle = makeHandle(au, 0, au.size());
        const int64_t ptsNs = 1500000000LL;

        const int sendsBefore = g_sendCalls;
        const jboolean accepted = Java_net_sourceforge_opencamera_OMTSender_nativeSendCoded(
                &env, nullptr, handle, 0, (jint)au.size(), OMTCodec_HEVC, 128, 72, 30, 1, ptsNs);
        ok(accepted == JNI_TRUE, "C1 a coded HEVC access unit is accepted");
        ok(g_sendCalls == sendsBefore + 1, "C1b and reached omt_send exactly once");
        ok(g_lastFrame.codec == Codec::HEVC, "C1c tagged 'HEVC'");
        ok(g_lastFrame.width == 128 && g_lastFrame.height == 72,
           "C1d geometry is the caller's (128x72), not a value captured at init");
        ok(g_lastFrame.frameRateN == 30 && g_lastFrame.frameRateD == 1,
           "C1e rate is the caller's (30/1)");
        ok(g_lastFrame.aspectRatio == 128.0f / 72.0f, "C1f aspect is width/height");
        ok(g_lastFrame.flags == OMTVideoFlags_None, "C1g no flags are claimed");
        ok(g_lastFrame.colorSpace == ColorSpace::BT601, "C1h a 72-line frame is 601, like the raw path");
        ok(g_lastFrame.timestamp == ptsNs / 100, "C1i nanoseconds became 100 ns ticks");
        ok(g_lastFrame.dataLength == (int)au.size(), "C1j the whole access unit is the payload");
        ok(g_lastFrame.data == static_cast<void*>(reinterpret_cast<FakeBuf*>(handle)->bytes.data()),
           "C1k the payload points at the caller's buffer");
        ok(g_lastFrame.data != nullptr && memcmp(g_lastFrame.data, au.data(), au.size()) == 0,
           "C1l and its bytes are the access unit's");

        Java_net_sourceforge_opencamera_OMTSender_nativeSendCoded(
                &env, nullptr, handle, 0, (jint)au.size(), OMTCodec_H264, 1920, 1080, 60, 1, ptsNs);
        ok(g_lastFrame.codec == OMTCodec_H264, "C2 an H.264 access unit keeps its own tag");
        ok(g_lastFrame.colorSpace == ColorSpace::BT709, "C2b and 1080 lines is 709");

        const int beforeBad = g_sendCalls;
        ok(Java_net_sourceforge_opencamera_OMTSender_nativeSendCoded(
                   &env, nullptr, handle, 0, (jint)au.size() + 1, OMTCodec_HEVC, 64, 64, 30, 1, ptsNs)
                   == JNI_FALSE, "C3 a length one past the capacity is refused");
        ok(g_sendCalls == beforeBad, "C3b nothing reached the sender");
        ok(has(g_log, "capacity"), "C3c and the log names the capacity it checked");
        ok(Java_net_sourceforge_opencamera_OMTSender_nativeSendCoded(
                   &env, nullptr, handle, 1, (jint)au.size(), OMTCodec_HEVC, 64, 64, 30, 1, ptsNs)
                   == JNI_FALSE, "C4 an offset that pushes the end past the capacity is refused");
        ok(Java_net_sourceforge_opencamera_OMTSender_nativeSendCoded(
                   &env, nullptr, handle, 0, 0, OMTCodec_HEVC, 64, 64, 30, 1, ptsNs) == JNI_FALSE,
           "C5 an empty access unit is refused");
        ok(Java_net_sourceforge_opencamera_OMTSender_nativeSendCoded(
                   &env, nullptr, handle, 0, (jint)au.size(), 0x12345678, 64, 64, 30, 1, ptsNs)
                   == JNI_FALSE, "C6 a tag that is neither HEVC nor H264 is refused");
        ok(Java_net_sourceforge_opencamera_OMTSender_nativeSendCoded(
                   &env, nullptr, handle, 0, (jint)au.size(), OMTCodec_HEVC, 64, 64, 30, 0, ptsNs)
                   == JNI_FALSE, "C7 frameRateD 0 is refused (the receiver divides the timeline by it)");
        ok(g_sendCalls == beforeBad, "C8 none of the refused frames reached the sender");
    }

    Java_net_sourceforge_opencamera_OMTSender_nativeCleanup(&env, nullptr);
    close(fd);

    std::printf("\njni-boundary: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
