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

#define LOG_TAG "OMTBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

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
// restarted stream reports its geometry again — see nativeSendFramePlanes.
static bool g_layoutLogged = false;

/**
 * Stamp one already-packed NV12 frame and hand it to the sender. Both JNI entry points funnel here
 * so the header the wire carries is built in exactly one place.
 *
 * CALLER MUST HOLD g_senderMutex (both do; this is a private helper, not an entry point).
 * `stride` and `dataLength` must be the geometry the packed buffer actually has — for the plane
 * path they come straight out of omtpack::packNv12(), which cannot report a length it did not write.
 */
static jboolean postFrame(JNIEnv* env, void* data, int stride, int64_t dataLength,
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
    
    return postFrame(env, data, yStride, need, width, height);
}

/**
 * Send one Camera2 YUV_420_888 frame, packing the planes into NV12 first.
 *
 * The planes arrive as the HAL described them; omtpack::packNv12() reads each one with ITS OWN
 * rowStride and pixelStride and writes a real NV12 buffer — interleaved Cb/Cr, UV plane at
 * stride*height — into `dst`, which the caller allocated with nv12BufferSize(). What the sender is
 * told (Stride, DataLength) is what the packer actually wrote, so the two cannot disagree and
 * Sender::validateFrame() has nothing left to refuse.
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

    uint8_t* dst = static_cast<uint8_t*>(env->GetDirectBufferAddress(dstBuffer));
    const uint8_t* yp = static_cast<const uint8_t*>(env->GetDirectBufferAddress(yBuffer));
    const uint8_t* up = static_cast<const uint8_t*>(env->GetDirectBufferAddress(uBuffer));
    const uint8_t* vp = static_cast<const uint8_t*>(env->GetDirectBufferAddress(vBuffer));
    if (dst == nullptr || yp == nullptr || up == nullptr || vp == nullptr) {
        LOGE("Frame refused: a plane buffer is not a direct ByteBuffer (dst=%p y=%p u=%p v=%p)",
             (void*)dst, (void*)yp, (void*)up, (void*)vp);
        return JNI_FALSE;
    }

    omtpack::Plane y{yp, ySize, yRowStride, yPixelStride};
    omtpack::Plane u{up, uSize, uRowStride, uPixelStride};
    omtpack::Plane v{vp, vSize, vRowStride, vPixelStride};
    omtpack::Result packed{};
    char why[192] = {};

    if (!omtpack::packNv12(y, u, v, width, height, dst,
                           env->GetDirectBufferCapacity(dstBuffer), &packed,
                           why, static_cast<int>(sizeof(why)))) {
        // Same discipline as the sender's gate: refuse, say why, do not read it further.
        LOGE("Frame refused before packing: %s (y: row=%d pix=%d size=%lld; u: row=%d pix=%d "
             "size=%lld; v: row=%d pix=%d size=%lld; %dx%d)",
             why, yRowStride, yPixelStride, (long long)ySize, uRowStride, uPixelStride,
             (long long)uSize, vRowStride, vPixelStride, (long long)vSize, width, height);
        return JNI_FALSE;
    }

    // One line per stream, not per frame: this is the number that cost a day to obtain, and it is
    // the only thing in the log that says what the HAL actually reported.
    if (!g_layoutLogged) {
        g_layoutLogged = true;
        LOGI("Camera2 chroma layout: %s — y rowStride=%d pixelStride=%d, uv rowStride=%d/%d, "
             "packed NV12 stride=%d dataLength=%lld for %dx%d",
             omtpack::layoutName(packed.layout), yRowStride, yPixelStride,
             uRowStride, vRowStride, packed.stride, (long long)packed.dataLength, width, height);
    }

    return postFrame(env, dst, packed.stride, packed.dataLength, width, height);
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
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    
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
