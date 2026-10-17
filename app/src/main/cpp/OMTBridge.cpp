/**
 * OMTBridge.cpp - JNI Bridge for Open Media Transport (Open Camera Edition)
 * 
 * This file provides the native interface between Java and the OMT C library.
 * It handles video frame sending from the Android camera to OMT receivers on the network.
 * 
 * Package: net.sourceforge.opencamera
 */

#include <jni.h>
#include <android/log.h>
#include <string>
#include <mutex>

#include "libomt.h"
#include "omtnv12.h"
#include <chrono>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#define LOG_TAG "OMTBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

#ifndef NDEBUG
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#define LOGD(...) ((void)0)
#endif

// Global sender instance (thread-safe access)
static omt_send_t* g_sender = nullptr;
static std::mutex g_senderMutex;

// Frame configuration (set during init, used during send)
static int g_width = 0;
static int g_height = 0;
static int g_frameRateN = 30;
static int g_frameRateD = 1;

// Whether the chroma layout of the current stream has been logged yet. Reset on init/cleanup so a
// restarted stream reports its geometry again — see nativeSendFramePlanes and the encoder thread,
// which is now the party that packs (and therefore the party that logs). Atomic because init/cleanup
// clear it on the UI thread while the encoder thread reads it.
static std::atomic<bool> g_layoutLogged{false};

/**
 * Stamp one already-packed NV12 frame and hand it to the sender. Every entry point funnels here
 * so the header the wire carries is built in exactly one place.
 *
 * CALLER MUST HOLD g_senderMutex (both do; this is a private helper, not an entry point).
 * `stride` and `dataLength` must be the geometry the packed buffer actually has — for the plane
 * path they come straight out of omtpack::packNv12(), which cannot report a length it did not write.
 *
 * No JNIEnv parameter on purpose: this runs on the encoder thread as well as on the JNI caller,
 * and a JNIEnv belongs to the thread that received it. It never touched one, and now it cannot.
 */
static jboolean postFrame(void* data, int stride, int64_t dataLength,
                          int width, int height) {
    OMTMediaFrame frame = {};

    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    int64_t timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count() / 100;

    frame.Type = OMTFrameType_Video;
    frame.Codec = OMTCodec_NV12;
    frame.Width = width;
    frame.Height = height;
    frame.Stride = stride;
    frame.Flags = OMTVideoFlags_None;
    frame.FrameRateN = g_frameRateN;
    frame.FrameRateD = g_frameRateD;
    frame.AspectRatio = (float)width / (float)height;
    frame.ColorSpace = (height >= 720) ? OMTColorSpace_BT709 : OMTColorSpace_BT601;
    frame.Timestamp = timestamp;

    frame.Data = data;
    // int, not int64: OMTMediaFrame::DataLength is an int and validateFrame() rejects a negative
    // one. The packer's own capacity checks keep every value that reaches here inside INT_MAX.
    frame.DataLength = (int)dataLength;

    // Send the frame
    int result = omt_send(g_sender, &frame);

    if (result != 0) {
        LOGD("Frame sent (result: %d)", result);
    }

    // Was: return JNI_TRUE unconditionally, so OMTSender.sendFrame()'s boolean could not
    // distinguish "a receiver got this" from "the codec refused it". omt_send returns 1 when
    // there are simply no subscribers, which is not a failure, so the test is > 0, not != 0.
    return result > 0 ? JNI_TRUE : JNI_FALSE;
}


/* ═══════════════════════════════════════════════════════════════════════════════════════════
 * THE FRAME RING — pack, encode and send no longer run on the camera callback
 * ═══════════════════════════════════════════════════════════════════════════════════════════
 *
 * THE FAILURE THIS FIXES. OMTStreamingManager.imageListener runs on the ImageReader's callback
 * thread, and until now the whole frame path ran inside it: JNI → nativeSendFramePlanes → packNv12
 * → omt_send → Sender::send → encodeFrame → VMX_EncodeP216, which pushes its slice ranges and then
 * Joins every one of them before it returns. The callback therefore costs a full encode, and
 * ImageReader was created with maxImages=3: the camera cannot recycle a buffer until the callback
 * closes it, so the delivered frame rate is 1/(per-frame cost of that callback) whatever the camera
 * or the declared frame rate asks for. Measured on one rig, both ways: the native x86_64 emulator
 * build gates green at 30.4/30.2 fps, the same build running under arm64 translation delivers
 * 11-13 fps. The declared rate never changed; only the callback's cost did. That is the whole of the
 * "~14 fps delivered vs 30 declared" report.
 *
 * WHAT CHANGED. The callback now does the one thing that must happen on its thread — copy the plane
 * bytes the Image is pointing at, because the moment it returns the HAL may overwrite them — and
 * closes the Image. packNv12, omt_send and everything behind them run on a dedicated encoder thread
 * fed by this ring.
 *
 * THE INVARIANTS THIS SECTION EXISTS TO KEEP, and where each is proven (qa/build-host.sh, the
 * et-* section, which drives the REAL translation unit below through qa/host-jni/jni.h with a
 * link-time codec that sleeps 70 ms per frame):
 *
 *   1. The callback returns in microseconds, not in encodes. Proven by the callback-latency
 *      assertion; et-prefix (the same binary against the pre-decoupling OMTBridge.cpp pinned at
 *      ET_PREFIX_REV) is red on exactly that line.
 *   2. The #31 JNI admission checks still fire, and they now belong to the COPY step — the copy is
 *      where the plane bytes are actually read, so it is where a caller's overstatement must be
 *      refused. qa/jni-boundary.cpp still drives this translation unit and jb-mutant-no-admission
 *      still dies under ASan, because admitAndCopyPlanes() below is the only code that reads a plane.
 *   3. A frame's destination buffer belongs to its ring slot. One shared frameBuffer reused across
 *      frames would let frame N+1's pack overwrite bytes frame N is still handing to the socket.
 *      Proven by the distinct-destination assertion; et-mutant-shared-dst restores the shared
 *      buffer and is caught by it.
 *   4. FIFO, always. The ring is a queue, the encoder claims from the front, and an eviction takes
 *      the OLDEST queued frame — never the one in flight, never out of order. Proven by the
 *      per-frame payload oracle over the delivered sequence; et-mutant-reorder and
 *      et-mutant-no-drop-oldest are caught by it.
 *   5. A drop is counted, not silent. g_ringDropped is the number; it is exposed through
 *      nativeGetRingDrops and logged when it moves, and the suite asserts
 *      delivered + dropped == admitted. et-mutant-counter-double-count is caught by that equation.
 *   6. Teardown joins the encoder thread. nativeCleanup stops it, and the stop DRAINS (every frame
 *      already admitted is delivered, in order) before the thread exits, so a restart cannot see a
 *      frame from the previous stream. The join must not happen while g_senderMutex is held: the
 *      encoder takes that mutex to post a frame, so holding it across the join is a deadlock with
 *      no diagnostic — which is exactly what et-mutant-join-under-sender-lock is, and what the
 *      bounded-teardown assertion catches (the scenario hangs and the harness scores it red, the
 *      same shape as the #24 stop-deadlock gate).
 *
 * Locking, in one line: g_ringMtx guards the ring and is never held while taking g_senderMutex;
 * g_senderMutex guards the sender and is never held while taking g_ringMtx. The producer takes only
 * the first, the encoder takes the first, releases it, then takes the second.
 */
namespace {

/* Same depth as the ImageReader's maxImages (OMTStreamingManager.java:226). Three frames in flight
 * is what the camera is allowed to have outstanding anyway; anything past that is a queue the
 * encoder thread has already decided it cannot catch up with, and the newest frame is the one a
 * viewer wants. */
constexpr size_t OMT_RING_DEPTH = 3;

enum : uint8_t { SlotEmpty = 0, SlotFilling = 1, SlotQueued = 2, SlotBusy = 3 };

/**
 * One frame's plane bytes, copied out of the buffers the JVM handed us.
 *
 * The chroma pair lives in ONE allocation, at the two offsets the Image reported them at, when the
 * two windows really do share bytes. That is not a convenience: omtpack::planChroma() decides the
 * chroma layout from the ADDRESS RELATIONSHIP between the Cb and Cr planes — off == +1 is CB_CR,
 * off == -1 is CR_CB, off == 0 is malformed and refused, and no overlap at all is two separate
 * stride-2 allocations. Copying the two windows into two separate buffers would silently turn every
 * semi-planar device into the SeparateStride2 case and quietly delete the off == 0 refusal. So the
 * copy preserves the geometry the packer reads, byte for byte and offset for offset.
 */
struct PlaneCopy {
    std::vector<uint8_t> y;        // the luma window, verbatim
    std::vector<uint8_t> chroma;   // the Cb and Cr windows, at uOff / vOff inside ONE allocation
    size_t uOff = 0, vOff = 0;
    int ySize = 0, uSize = 0, vSize = 0;
    int yRowStride = 0, yPixelStride = 1;
    int uRowStride = 0, uPixelStride = 1;
    int vRowStride = 0, vPixelStride = 1;
    int width = 0, height = 0;

    const uint8_t* uPtr() const { return chroma.data() + uOff; }
    const uint8_t* vPtr() const { return chroma.data() + vOff; }

    void clear() {
        y.clear(); chroma.clear();
        uOff = vOff = 0;
        ySize = uSize = vSize = 0;
        width = height = 0;
    }
};

struct RingSlot {
    PlaneCopy copy;                    // written by the producer, read by the encoder while Busy
    std::vector<uint8_t> packed;       // INVARIANT 3: this slot's own NV12 destination
    unsigned long long seq = 0;        // admission order, for the log and the suite
};

RingSlot g_ring[OMT_RING_DEPTH];
uint8_t g_ringState[OMT_RING_DEPTH] = {SlotEmpty, SlotEmpty, SlotEmpty};
size_t g_ringOrder[OMT_RING_DEPTH] = {0, 0, 0};   // FIFO of queued slot indices, front = oldest
size_t g_ringOrderLen = 0;
std::mutex g_ringMtx;
std::condition_variable g_ringCv;
std::thread g_encoderThread;
bool g_encoderStop = false;                       // guarded by g_ringMtx
bool g_encoderLive = false;                       // guarded by g_ringMtx

/* INVARIANT 5. The counters a viewer of the bug needs: how many frames were admitted, how many the
 * ring had to evict to admit them, how many reached the sender, and how many the packer refused. */
std::atomic<unsigned long long> g_ringAdmitted{0};
std::atomic<unsigned long long> g_ringDropped{0};
std::atomic<unsigned long long> g_ringDelivered{0};
std::atomic<unsigned long long> g_ringPackRefused{0};
std::atomic<bool> g_senderAlive{false};

/** Grow a byte vector to exactly `n`, keeping the allocation across frames: the callback thread
 *  must not malloc 3 MB per frame on a phone that is already short of one. */
void resizeBytes(std::vector<uint8_t>& v, size_t n) {
    if (v.size() < n) v.resize(n);
}

/**
 * The #31 admission gate and the copy it now owns. Returns false — having logged WHY, naming the
 * buffer — without reading a single byte of the frame if any claimed length fails to agree with
 * GetDirectBufferCapacity() of the buffer it came from.
 *
 * This is the only code in the bridge that reads a plane buffer. Everything downstream — the
 * encoder thread, the packer, the sender — reads `out`, which this function filled.
 */
bool admitAndCopyPlanes(JNIEnv* env,
                        jobject dstBuffer,
                        jobject yBuffer, jint ySize, jint yRowStride, jint yPixelStride,
                        jobject uBuffer, jint uSize, jint uRowStride, jint uPixelStride,
                        jobject vBuffer, jint vSize, jint vRowStride, jint vPixelStride,
                        jint width, jint height,
                        PlaneCopy& out, const char* entryName) {
    uint8_t* dst = nullptr;
    if (dstBuffer != nullptr) {
        dst = static_cast<uint8_t*>(env->GetDirectBufferAddress(dstBuffer));
    }
    const uint8_t* yp = static_cast<const uint8_t*>(env->GetDirectBufferAddress(yBuffer));
    const uint8_t* up = static_cast<const uint8_t*>(env->GetDirectBufferAddress(uBuffer));
    const uint8_t* vp = static_cast<const uint8_t*>(env->GetDirectBufferAddress(vBuffer));
    /* A caller with no destination (the queue path: the ring slot IS its destination) must not be
     * refused for the buffer it did not pass. Every plane still has to be a real direct buffer. */
    if ((dstBuffer != nullptr && dst == nullptr) || yp == nullptr || up == nullptr || vp == nullptr) {
        LOGE("Frame refused: a plane buffer is not a direct ByteBuffer (dst=%p y=%p u=%p v=%p)",
             (void*)dst, (void*)yp, (void*)up, (void*)vp);
        return false;
    }

    /* ---- vet the caller's three length claims against the buffers they came from ----
     *
     * ySize/uSize/vSize arrive as plain ints, and every bounds check inside packNv12() is of the form
     * `plane.size >= need`: they measure the geometry against the CALLER's word, not against memory.
     * A Java side that computed the length from its own idea of the plane (rather than passing
     * getBuffer().limit()), a buffer reused after a resize, or a future refactor that forgets to
     * re-read the Image would each hand over a number bigger than the allocation, and the packer's
     * checks would all nod and then read past the end of it. The destination was never allowed to lie
     * like this — GetDirectBufferCapacity(dstBuffer) is what gates it — so make the sources answer to
     * the same authority. The JVM knows how long each buffer is; ask the JVM, not Java.
     *
     * This must run before the first read, and it must say which buffer it refused. It now runs
     * before the first COPY, which is the same sentence said differently only because the copy is
     * the first read: nothing below this loop may touch yp/up/vp. */
    const int64_t yCap = env->GetDirectBufferCapacity(yBuffer);
    const int64_t uCap = env->GetDirectBufferCapacity(uBuffer);
    const int64_t vCap = env->GetDirectBufferCapacity(vBuffer);

    char why[256] = {};
    const omtpack::SourceClaim claims[3] = {
        omtpack::SourceClaim{yp, ySize, yCap},
        omtpack::SourceClaim{up, uSize, uCap},
        omtpack::SourceClaim{vp, vSize, vCap},
    };
    const char* const planeNames[3] = { "Y plane (planes[0])", "Cb plane (planes[1])",
                                        "Cr plane (planes[2])" };
    for (int i = 0; i < 3; ++i) {
        if (!omtpack::admitSource(claims[i], planeNames[i], why, static_cast<int>(sizeof(why)))) {
            LOGE("Frame refused before packing: %s (y: row=%d pix=%d size=%lld cap=%lld; "
                 "u: row=%d pix=%d size=%lld cap=%lld; v: row=%d pix=%d size=%lld cap=%lld; %dx%d) [%s]",
                 why, yRowStride, yPixelStride, (long long)ySize, (long long)yCap,
                 uRowStride, uPixelStride, (long long)uSize, (long long)uCap,
                 vRowStride, vPixelStride, (long long)vSize, (long long)vCap, width, height,
                 entryName);
            return false;
        }
    }

    /* ---- the copy itself: exactly the three admitted windows, nothing else ---- */
    resizeBytes(out.y, (size_t)ySize);
    std::memcpy(out.y.data(), yp, (size_t)ySize);

    /* Do the two chroma windows share bytes? The same strict-interval test planChroma() uses, on the
     * addresses the caller actually handed over. If they do, they are ONE allocation described twice
     * and the copy must be one allocation with the same relative offsets — see the PlaneCopy comment
     * for what a separate copy of each would silently un-decide. */
    const uintptr_t ua = reinterpret_cast<uintptr_t>(up);
    const uintptr_t va = reinterpret_cast<uintptr_t>(vp);
    const bool sharesBytes = (ua < va + (uintptr_t)vSize) && (va < ua + (uintptr_t)uSize);
    if (sharesBytes) {
        const uintptr_t lo = std::min(ua, va);
        const uintptr_t hi = std::max(ua + (uintptr_t)uSize, va + (uintptr_t)vSize);
        resizeBytes(out.chroma, (size_t)(hi - lo));
        std::memcpy(out.chroma.data(), reinterpret_cast<const void*>(lo), (size_t)(hi - lo));
        out.uOff = (size_t)(ua - lo);
        out.vOff = (size_t)(va - lo);
    } else {
        resizeBytes(out.chroma, (size_t)uSize + (size_t)vSize);
        std::memcpy(out.chroma.data(), up, (size_t)uSize);
        std::memcpy(out.chroma.data() + uSize, vp, (size_t)vSize);
        out.uOff = 0;
        out.vOff = (size_t)uSize;
    }

    out.ySize = ySize; out.uSize = uSize; out.vSize = vSize;
    out.yRowStride = yRowStride; out.yPixelStride = yPixelStride;
    out.uRowStride = uRowStride; out.uPixelStride = uPixelStride;
    out.vRowStride = vRowStride; out.vPixelStride = vPixelStride;
    out.width = width; out.height = height;
    return true;
}

/** Pack an admitted copy into `dst`, which must be at least nv12Size(yRowStride, height) bytes.
 *  The packer's own capacity gate is what refuses a destination that is too small — the same call,
 *  with the same reason string, that nativeSendFramePlanes has always made. */
bool packCopy(const PlaneCopy& src, uint8_t* dst, int64_t dstCapacity,
              omtpack::Result& packed, char* why, int whyCap) {
    omtpack::Plane y{src.y.data(), src.ySize, src.yRowStride, src.yPixelStride};
    omtpack::Plane u{src.uPtr(), src.uSize, src.uRowStride, src.uPixelStride};
    omtpack::Plane v{src.vPtr(), src.vSize, src.vRowStride, src.vPixelStride};
    return omtpack::packNv12(y, u, v, src.width, src.height, dst, dstCapacity, &packed,
                             why, whyCap);
}

void logLayoutOnce(const PlaneCopy& src, const omtpack::Result& packed) {
    if (g_layoutLogged.exchange(true)) return;
    // One line per stream, not per frame: this is the number that cost a day to obtain, and it is
    // the only thing in the log that says what the HAL actually reported.
    LOGI("Camera2 chroma layout: %s — y rowStride=%d pixelStride=%d, uv rowStride=%d/%d, "
         "packed NV12 stride=%d dataLength=%lld for %dx%d",
         omtpack::layoutName(packed.layout), src.yRowStride, src.yPixelStride,
         src.uRowStride, src.vRowStride, packed.stride, (long long)packed.dataLength,
         src.width, src.height);
}

/** Pack a slot and post it. CALLER MUST NOT HOLD g_senderMutex; this takes it itself. */
bool encodeAndPostSlot(RingSlot& slot) {
    std::lock_guard<std::mutex> lock(g_senderMutex);
    if (g_sender == nullptr) {
        // Teardown won the race. The frame is gone and the drop counter is the only place it can
        // be reported — a silent loss here is the bug b66ab347 was written against.
        LOGW("OMT frame ring: sender is gone; frame %llu was never encoded", slot.seq);
        g_ringPackRefused.fetch_add(1);
        return false;
    }
    const int64_t need = omtpack::nv12Size(slot.copy.yRowStride, slot.copy.height);
    if (need < 0) {
        LOGE("Frame %llu refused: cannot size a packed NV12 buffer for stride=%d height=%d",
             slot.seq, slot.copy.yRowStride, slot.copy.height);
        g_ringPackRefused.fetch_add(1);
        return false;
    }
    resizeBytes(slot.packed, (size_t)need);
    omtpack::Result packed{};
    char why[256] = {};
    if (!packCopy(slot.copy, slot.packed.data(), (int64_t)slot.packed.size(), packed,
                  why, static_cast<int>(sizeof(why)))) {
        LOGE("Frame %llu refused before packing: %s (y: row=%d pix=%d size=%d; u: row=%d pix=%d "
             "size=%d; v: row=%d pix=%d size=%d; %dx%d)",
             slot.seq, why, slot.copy.yRowStride, slot.copy.yPixelStride, slot.copy.ySize,
             slot.copy.uRowStride, slot.copy.uPixelStride, slot.copy.uSize,
             slot.copy.vRowStride, slot.copy.vPixelStride, slot.copy.vSize,
             slot.copy.width, slot.copy.height);
        g_ringPackRefused.fetch_add(1);
        return false;
    }
    logLayoutOnce(slot.copy, packed);
    const jboolean sent = postFrame(slot.packed.data(), packed.stride, packed.dataLength,
                                    slot.copy.width, slot.copy.height);
    if (sent == JNI_TRUE) g_ringDelivered.fetch_add(1);
    return sent == JNI_TRUE;
}

void encoderLoop() {
    for (;;) {
        int idx = -1;
        {
            std::unique_lock<std::mutex> lk(g_ringMtx);
            g_ringCv.wait(lk, [] { return g_encoderStop || g_ringOrderLen > 0; });
            if (g_ringOrderLen > 0) {
                // INVARIANT 4: claim from the FRONT. The back is where the newest frame went.
                idx = (int)g_ringOrder[0];
                for (size_t i = 1; i < g_ringOrderLen; ++i) g_ringOrder[i - 1] = g_ringOrder[i];
                --g_ringOrderLen;
                g_ringState[idx] = SlotBusy;
            } else if (g_encoderStop) {
                return;   // INVARIANT 6: stop drains — the queue is empty and nothing is in flight
            } else {
                continue;  // spurious wakeup: the predicate is the truth, not the notify
            }
        }
        RingSlot& slot = g_ring[idx];
        encodeAndPostSlot(slot);
        {
            std::lock_guard<std::mutex> lk(g_ringMtx);
            g_ringState[idx] = SlotEmpty;   // the producer may recycle it now
        }
    }
}

void startEncoderLocked() {
    if (g_encoderLive) return;
    g_encoderStop = false;
    g_encoderLive = true;
    g_encoderThread = std::thread(encoderLoop);
}

/**
 * Stop and join the encoder thread. NEVER call this while holding g_senderMutex — the thread takes
 * that mutex inside encodeAndPostSlot(), so a join under it is a deadlock that looks like a hung
 * app. (qa/build-host.sh's et-mutant-join-under-sender-lock is that exact one-line mistake.)
 */
void stopEncoderThread() {
    std::thread victim;
    {
        std::lock_guard<std::mutex> lk(g_ringMtx);
        if (!g_encoderLive) return;
        g_encoderStop = true;
        victim = std::move(g_encoderThread);
        g_encoderLive = false;
    }
    g_ringCv.notify_all();
    if (victim.joinable()) victim.join();
    // The thread is gone. Anything still in the ring at this point was admitted AFTER the stop was
    // requested — a producer that raced teardown — and a frame that entered the ring and never
    // reached the sender must be counted, not forgotten. Same counter as the evictions, because the
    // operator's question is identical: "how many frames went in the front and did not come out".
    {
        std::lock_guard<std::mutex> lk(g_ringMtx);
        if (g_ringOrderLen > 0) {
            const unsigned long long lost = (unsigned long long)g_ringOrderLen;
            g_ringDropped.fetch_add(lost);
            LOGW("OMT frame ring: %llu queued frame(s) were discarded when the encoder thread stopped",
                 lost);
        }
        g_ringOrderLen = 0;
        for (size_t i = 0; i < OMT_RING_DEPTH; ++i) g_ringState[i] = SlotEmpty;
    }
}

} // namespace

extern "C" {

/**
 * Initialize the OMT sender with the given name and video configuration.
 */
JNIEXPORT jboolean JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeInit(
        JNIEnv* env,
        jobject /* this */,
        jstring name,
        jint width,
        jint height,
        jint frameRateN,
        jint frameRateD,
        jint quality) {

    // A restarted stream must not inherit the previous stream's encoder thread, its queue, or the
    // frames still sitting in it. Stop and join OUT HERE, before the sender mutex: the encoder thread
    // takes g_senderMutex to post a frame, so joining under it would deadlock (see stopEncoderThread).
    stopEncoderThread();
    g_senderAlive.store(false, std::memory_order_release);   // until this init succeeds, nothing may queue

    std::lock_guard<std::mutex> lock(g_senderMutex);
    
    // Cleanup existing sender if any
    if (g_sender != nullptr) {
        LOGI("Destroying existing sender before creating new one");
        omt_send_destroy(g_sender);
        g_sender = nullptr;
    }
    
    // Get the sender name from Java string
    const char* senderName = env->GetStringUTFChars(name, nullptr);
    if (senderName == nullptr) {
        LOGE("Failed to get sender name string");
        return JNI_FALSE;
    }
    
    LOGI("Creating OMT sender: %s (%dx%d @ %d/%d fps, Quality: %d)", 
         senderName, width, height, frameRateN, frameRateD, quality);
    
    // Create the OMT sender with specified quality
    g_sender = omt_send_create(senderName, (OMTQuality)quality);
    
    env->ReleaseStringUTFChars(name, senderName);
    
    if (g_sender == nullptr) {
        LOGE("Failed to create OMT sender");
        return JNI_FALSE;
    }
    
    // Store configuration for frame sending
    g_width = width;
    g_height = height;
    g_frameRateN = frameRateN;
    g_frameRateD = frameRateD;
    g_layoutLogged = false;   // the next stream reports the layout its HAL actually gave us
    // A new sender is a new stream: the ring's tally describes THIS sender's life, and a counter
    // carried over from the previous one would blame the new stream for the old stream's stalls.
    g_ringAdmitted = 0;
    g_ringDropped = 0;
    g_ringDelivered = 0;
    g_ringPackRefused = 0;
    g_senderAlive.store(true, std::memory_order_release);

    LOGI("OMT sender created successfully");
    return JNI_TRUE;
}

/**
 * Send a video frame to connected receivers.
 *
 * DEPRECATED path: the caller hands over a buffer it claims is already NV12. YUV_420_888 does not
 * guarantee that — see omtnv12.h — and the capture path now goes through nativeSendFramePlanes,
 * which packs the three planes itself. This entry point stays for callers that genuinely hold an
 * NV12 buffer, but it no longer trusts the strides it is given: the frame is charged for
 * stride*height*3/2 (what the encoder reads), so a caller that sized its buffer with a separate
 * uv rowStride is refused here with a reason instead of reaching the sender's gate with a lie.
 */
JNIEXPORT jboolean JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeSendFrame(
        JNIEnv* env,
        jobject /* this */,
        jobject buffer,
        jint width,
        jint height,
        jint yStride,
        jint uvStride) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    
    if (g_sender == nullptr) {
        LOGE("Cannot send frame: sender not initialized");
        return JNI_FALSE;
    }
    
    // Get the native buffer address
    void* data = env->GetDirectBufferAddress(buffer);
    if (data == nullptr) {
        LOGE("Failed to get direct buffer address");
        return JNI_FALSE;
    }
    
    // Calculate data length for NV12 format. The encoder walks BOTH planes with yStride
    // (OMTSender.cpp: encodeFrame() passes frame.Stride as srcStrideY and srcStrideUV), so anything
    // the caller allocated with a narrower uv rowStride cannot honour that read. (height/2)*yStride
    // is the number of chroma bytes the codec will touch; (height/2)*uvStride is what the caller
    // thinks it reserved — the gap between them is the defect that made this path unusable.
    const int64_t need = omtpack::nv12Size(yStride, height);
    const int64_t claimed = (static_cast<int64_t>(height) * yStride)
                          + ((static_cast<int64_t>(height) / 2) * uvStride);
    if (need < 0 || claimed < need || env->GetDirectBufferCapacity(buffer) < need) {
        LOGE("Cannot send frame: NV12 buffer cannot honour stride=%d height=%d — the encoder reads "
             "%lld bytes, the caller described %lld (uv rowStride %d < y rowStride %d). Use "
             "sendFramePlanes() for a Camera2 Image.",
             yStride, height, (long long)need, (long long)claimed, uvStride, yStride);
        return JNI_FALSE;
    }
    
    return postFrame(data, yStride, need, width, height);
}

/**
 * Send one Camera2 YUV_420_888 frame SYNCHRONOUSLY, packing the planes into NV12 first.
 *
 * The planes arrive as the HAL described them; admitAndCopyPlanes() vets each caller's claimed length
 * against GetDirectBufferCapacity() and copies the window, omtpack::packNv12() reads that copy with
 * each plane's OWN rowStride and pixelStride and writes a real NV12 buffer — interleaved Cb/Cr, UV
 * plane at stride*height — into `dst`, which the caller allocated with nv12BufferSize(). What the
 * sender is told (Stride, DataLength) is what the packer actually wrote, so the two cannot disagree
 * and Sender::validateFrame() has nothing left to refuse.
 *
 * SYNCHRONOUS means the caller waits for the encode: this is the entry point for a caller that WANTS
 * backpressure, and it is the one qa/jni-boundary.cpp drives. The camera path is
 * nativeQueueFramePlanes below — an ImageReader callback must never wait for an encode, which is the
 * whole subject of the frame-ring comment above.
 */
JNIEXPORT jboolean JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeSendFramePlanes(
        JNIEnv* env,
        jobject /* this */,
        jobject dstBuffer,
        jobject yBuffer, jint ySize, jint yRowStride, jint yPixelStride,
        jobject uBuffer, jint uSize, jint uRowStride, jint uPixelStride,
        jobject vBuffer, jint vSize, jint vRowStride, jint vPixelStride,
        jint width, jint height) {

    std::lock_guard<std::mutex> lock(g_senderMutex);

    if (g_sender == nullptr) {
        LOGE("Cannot send frame: sender not initialized");
        return JNI_FALSE;
    }

    PlaneCopy src;
    if (!admitAndCopyPlanes(env, dstBuffer,
                            yBuffer, ySize, yRowStride, yPixelStride,
                            uBuffer, uSize, uRowStride, uPixelStride,
                            vBuffer, vSize, vRowStride, vPixelStride,
                            width, height, src, "nativeSendFramePlanes")) {
        return JNI_FALSE;   // admitAndCopyPlanes named the buffer and the reason
    }

    uint8_t* dst = static_cast<uint8_t*>(env->GetDirectBufferAddress(dstBuffer));
    omtpack::Result packed{};
    char why[256] = {};
    if (!packCopy(src, dst, env->GetDirectBufferCapacity(dstBuffer), packed,
                  why, static_cast<int>(sizeof(why)))) {
        // Same discipline as the sender's gate: refuse, say why, do not read it further.
        LOGE("Frame refused before packing: %s (y: row=%d pix=%d size=%lld; u: row=%d pix=%d "
             "size=%lld; v: row=%d pix=%d size=%lld; %dx%d)",
             why, yRowStride, yPixelStride, (long long)ySize, uRowStride, uPixelStride,
             (long long)uSize, vRowStride, vPixelStride, (long long)vSize, width, height);
        return JNI_FALSE;
    }

    logLayoutOnce(src, packed);

    return postFrame(dst, packed.stride, packed.dataLength, width, height);
}

/**
 * Hand one Camera2 YUV_420_888 frame to the encoder thread and come back.
 *
 * This is the capture path's entry point (OMTStreamingManager.sendFrame). It does the two things
 * that must happen before the Image is closed — vet the caller's plane claims and copy the plane
 * bytes — and nothing else: no packing, no encode, no socket, and no g_senderMutex, so its cost is
 * a memcpy and its return does not depend on how long the codec takes. The dedicated encoder thread
 * packs and sends, in the order the frames arrived.
 *
 * The ring holds OMT_RING_DEPTH frames. When it is full the OLDEST queued frame is evicted — never
 * the one in flight, never the newest — and g_ringDropped counts every eviction, because a drop that
 * nobody can see is the bug the sender's own counters were written against.
 *
 * @return true if THIS frame was admitted to the ring; false if it was refused (bad geometry, a lie
 *         about a buffer, or no sender). JNI_TRUE does not mean a receiver took it — see
 *         nativeGetRingDrops / the sender's own delivered counters for that.
 */
JNIEXPORT jboolean JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeQueueFramePlanes(
        JNIEnv* env,
        jobject /* this */,
        jobject yBuffer, jint ySize, jint yRowStride, jint yPixelStride,
        jobject uBuffer, jint uSize, jint uRowStride, jint uPixelStride,
        jobject vBuffer, jint vSize, jint vRowStride, jint vPixelStride,
        jint width, jint height) {

    if (!g_senderAlive.load(std::memory_order_acquire)) {
        LOGE("Cannot queue frame: sender not initialized");
        return JNI_FALSE;
    }

    // Refuse a geometry the packer could never honour BEFORE spending a slot and a copy on it.
    const int64_t need = omtpack::nv12Size(yRowStride, height);
    if (need < 0) {
        LOGE("Frame refused before copying: cannot size a packed NV12 buffer for stride=%d "
             "height=%d — the frame is not packable", yRowStride, height);
        return JNI_FALSE;
    }

    std::lock_guard<std::mutex> lk(g_ringMtx);
    startEncoderLocked();

    int idx = -1;
    for (size_t i = 0; i < OMT_RING_DEPTH; ++i) {
        if (g_ringState[i] == SlotEmpty) { idx = (int)i; break; }
    }

    bool evicted = false;
    unsigned long long evictedSeq = 0;
    if (idx < 0) {
        // INVARIANT 4 + 5: full ring → the OLDEST QUEUED frame loses its slot to the newest one,
        // and the eviction is counted and logged. The slot the encoder thread is holding (SlotBusy)
        // is never touched: its bytes are on their way to a subscriber.
        if (g_ringOrderLen == 0) {
            LOGE("Frame ring has no empty slot and nothing queued — refusing the frame");
            return JNI_FALSE;
        }
        idx = (int)g_ringOrder[0];
        evictedSeq = g_ring[idx].seq;
        for (size_t i = 1; i < g_ringOrderLen; ++i) g_ringOrder[i - 1] = g_ringOrder[i];
        --g_ringOrderLen;
        evicted = true;
    }

    RingSlot& slot = g_ring[idx];
    g_ringState[idx] = SlotFilling;          // invisible to the encoder until it is complete
    slot.copy.clear();
    if (!admitAndCopyPlanes(env, nullptr,
                            yBuffer, ySize, yRowStride, yPixelStride,
                            uBuffer, uSize, uRowStride, uPixelStride,
                            vBuffer, vSize, vRowStride, vPixelStride,
                            width, height, slot.copy, "nativeQueueFramePlanes")) {
        g_ringState[idx] = SlotEmpty;        // nothing was read; the slot is as it was
        return JNI_FALSE;
    }
    slot.seq = g_ringAdmitted.fetch_add(1) + 1;
    g_ringState[idx] = SlotQueued;
    g_ringOrder[g_ringOrderLen++] = (size_t)idx;
    if (evicted) {
        const unsigned long long total = g_ringDropped.fetch_add(1) + 1;
        LOGW("OMT frame ring full (depth %zu): dropped the oldest queued frame (seq %llu, "
             "total dropped %llu, admitted %llu)",
             OMT_RING_DEPTH, evictedSeq, total, g_ringAdmitted.load());
    }
    g_ringCv.notify_one();
    return JNI_TRUE;
}

/**
 * Frames the ring evicted to keep the callback non-blocking since the sender was created.
 *
 * This is the number that distinguishes "the camera is producing 30 fps and the encoder is keeping
 * up" from "the camera is producing 30 fps and the ring is throwing two thirds of them away" — the
 * two look identical from the delivered rate alone. Reported alongside the sender's own drop
 * statistics so a network stall and an encode stall cannot be confused for each other.
 */
JNIEXPORT jlong JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeGetRingDrops(
        JNIEnv* /* env */, jobject /* this */) {
    return (jlong)g_ringDropped.load();
}


/**
 * Get the number of currently connected receivers.
 */
JNIEXPORT jint JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeGetConnectionCount(
        JNIEnv* env,
        jobject /* this */) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    
    if (g_sender == nullptr) {
        return 0;
    }
    
    return omt_send_connections(g_sender);
}

/**
 * Get the discovery address string for this sender.
 */
JNIEXPORT jstring JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeGetAddress(
        JNIEnv* env,
        jobject /* this */) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    
    if (g_sender == nullptr) {
        return nullptr;
    }
    
    char address[OMT_MAX_STRING_LENGTH];
    int length = omt_send_getaddress(g_sender, address, OMT_MAX_STRING_LENGTH);
    
    if (length <= 0) {
        return nullptr;
    }
    
    return env->NewStringUTF(address);
}

/**
 * Cleanup and destroy the OMT sender.
 */
JNIEXPORT void JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeCleanup(
        JNIEnv* env,
        jobject /* this */) {

    // INVARIANT 6, in two moves and in this order. First stop the encoder thread and WAIT for it —
    // no lock held, because the thread is inside encodeAndPostSlot() taking g_senderMutex, and a
    // join under that mutex would deadlock the app with nothing in the log. The stop drains, so
    // every frame the camera already handed us still reaches its subscribers.
    stopEncoderThread();

    // Only now is it safe to destroy what it was posting to.
    std::lock_guard<std::mutex> lock(g_senderMutex);

    g_senderAlive.store(false, std::memory_order_release);
    if (g_sender != nullptr) {
        LOGI("Destroying OMT sender");
        omt_send_destroy(g_sender);
        g_sender = nullptr;
    }

    g_width = 0;
    g_height = 0;
    g_layoutLogged = false;
}

// =============================================================
// Statistics API (for UI feedback on frame drops)
// =============================================================

// External C functions from libomt
extern int64_t omt_send_get_frames_sent(omt_send_t* instance);
extern int64_t omt_send_get_frames_dropped(omt_send_t* instance);
extern int64_t omt_send_get_recent_drops_and_reset(omt_send_t* instance);
extern int64_t omt_send_get_bytes_sent(omt_send_t* instance);
// Custom helper
extern void omt_send_set_info_str(omt_send_t* instance, const char* product, const char* manufacturer);
extern void omt_send_set_quality(omt_send_t* instance, OMTQuality quality);

/**
 * Set the current VMX quality level.
 */
JNIEXPORT void JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeSetQuality(
        JNIEnv* env,
        jobject /* this */,
        jint quality) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    if (g_sender == nullptr) return;
    
    LOGI("JNI: Setting OMT quality to %d", quality);
    omt_send_set_quality(g_sender, (OMTQuality)quality);
}

/**
 * Set sender information (product, manufacturer).
 */
JNIEXPORT void JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeSetSenderInfo(
        JNIEnv* env,
        jobject /* this */,
        jstring productName,
        jstring manufacturer) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    
    if (g_sender == nullptr) return;
    
    const char* productStr = env->GetStringUTFChars(productName, nullptr);
    const char* mfgStr = env->GetStringUTFChars(manufacturer, nullptr);
    
    if (productStr && mfgStr) {
        omt_send_set_info_str(g_sender, productStr, mfgStr);
    }
    
    if (productStr) env->ReleaseStringUTFChars(productName, productStr);
    if (mfgStr) env->ReleaseStringUTFChars(manufacturer, mfgStr);
}

/**
 * Get total frames sent since init.
 */
JNIEXPORT jlong JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeGetFramesSent(
        JNIEnv* env,
        jobject /* this */) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    if (g_sender == nullptr) return 0;
    return (jlong)omt_send_get_frames_sent(g_sender);
}

/**
 * Get total frames dropped since init.
 */
JNIEXPORT jlong JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeGetFramesDropped(
        JNIEnv* env,
        jobject /* this */) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    if (g_sender == nullptr) return 0;
    return (jlong)omt_send_get_frames_dropped(g_sender);
}

/**
 * Get recent drops and reset counter (for periodic UI updates).
 * Returns the number of frames dropped since last call.
 */
JNIEXPORT jlong JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeGetRecentDropsAndReset(
        JNIEnv* env,
        jobject /* this */) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    if (g_sender == nullptr) return 0;
    return (jlong)omt_send_get_recent_drops_and_reset(g_sender);
}

/**
 * Get total bytes sent since init.
 */
JNIEXPORT jlong JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeGetBytesSent(
        JNIEnv* env,
        jobject /* this */) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    if (g_sender == nullptr) return 0;
    return (jlong)omt_send_get_bytes_sent(g_sender);
}

extern int omt_send_get_profile(omt_send_t* instance);

/**
 * Get current VMX profile ID.
 */
JNIEXPORT jint JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeGetProfile(
        JNIEnv* env,
        jobject /* this */) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    if (g_sender == nullptr) return 0;
    return omt_send_get_profile(g_sender);
}

} // extern "C"
