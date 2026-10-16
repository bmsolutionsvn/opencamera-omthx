package net.sourceforge.opencamera;

import android.util.Log;
import java.nio.ByteBuffer;

/**
 * OMTSender - Java wrapper for the Open Media Transport sender.
 * 
 * This class provides a high-level interface to stream video frames
 * from the Android camera to OMT receivers on the local network.
 * 
 * Usage:
 * 1. Create an instance of OMTSender
 * 2. Call init() with your desired configuration
 * 3. Call sendFrame() for each camera frame
 * 4. Call cleanup() when done streaming
 */
public class OMTSender {

    private static final String TAG = "OMTSender";

    /**
     * Quality/Compression constants for OMT video codec (VMX 4:2:2)
     * 
     * Bandwidth reference at 1080p30:
     * - LOW: ~43 Mbps (Best for Wi-Fi stability)
     * - MEDIUM: ~100 Mbps (Requires good Wi-Fi 5/6)
     * - HIGH: ~130 Mbps (Requires Wi-Fi 6 or wired connection)
     */
    public static final int QUALITY_DEFAULT = 0; // Auto
    public static final int QUALITY_LOW = 1; // ~43 Mbps @ 1080p30 (Recommended for Wi-Fi)
    public static final int QUALITY_MEDIUM = 50; // ~100 Mbps @ 1080p30
    public static final int QUALITY_HIGH = 100; // ~130 Mbps @ 1080p30 (Wired/Wi-Fi 6 only)

    private boolean isInitialized = false;

    // Static block to load native libraries
    static {
        try {
            // Load the VMX codec library first (dependency)
            System.loadLibrary("vmx");
            Log.i(TAG, "Loaded libvmx.so");

            // Load the OMT library
            System.loadLibrary("omt");
            Log.i(TAG, "Loaded libomt.so");

            // Load our OMT bridge library with the JNI functions
            System.loadLibrary("omtbridge");
            Log.i(TAG, "Loaded libomtbridge.so");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "Failed to load native libraries: " + e.getMessage());
            throw e;
        }
    }

    /**
     * Initialize the OMT sender.
     * 
     * @param name      The name of the sender (visible in discovery)
     * @param width     Video width in pixels
     * @param height    Video height in pixels
     * @param frameRate Frame rate
     * @param quality   Video quality (0=Default, 1=Low, 50=Medium, 100=High)
     * @return true if initialization succeeded
     */
    public boolean init(String name, int width, int height, int frameRate, int quality) {
        if (isInitialized) {
            Log.w(TAG, "OMTSender already initialized, cleaning up first");
            cleanup();
        }

        isInitialized = nativeInit(name, width, height, frameRate, 1, quality);

        if (isInitialized) {
            Log.i(TAG, "OMT Sender initialized: " + name + " (" + width + "x" + height + " @ " + frameRate
                    + "fps, Quality: " + quality + ")");
        } else {
            Log.e(TAG, "Failed to initialize OMT Sender");
        }

        return isInitialized;
    }

    /**
     * Set additional sender information (e.g. device model).
     * Must be called after init().
     */
    public void setSenderInfo(String productName, String manufacturer) {
        if (isInitialized) {
            nativeSetSenderInfo(productName, manufacturer);
        }
    }

    /**
     * Initialize with default quality (MEDIUM).
     */
    public boolean init(String name, int width, int height, int frameRate) {
        return init(name, width, height, frameRate, QUALITY_MEDIUM);
    }

    /**
     * Send a video frame to connected receivers.
     * 
     * The buffer should contain NV12 formatted pixel data (Y plane followed by UV
     * plane).
     *
     * @deprecated ImageFormat.YUV_420_888 does not guarantee that layout — the chroma plane is
     *             interleaved (NV12) or planar (I420) depending on what the HAL reports, and a
     *             Camera2 device whose two row strides differ used to be refused by the sender's
     *             own gate on every frame. Callers holding an {@link android.media.Image} want
     *             {@link #sendFramePlanes}; this entry point now refuses a buffer whose capacity
     *             cannot honour {@code stride*height*3/2} rather than shipping the mismatch.
     *
     * @param buffer   Direct ByteBuffer containing NV12 pixel data
     * @param width    Frame width
     * @param height   Frame height
     * @param yStride  Stride of the Y plane (may include row padding)
     * @param uvStride Stride of the UV plane
     * @return true if frame was sent successfully
     */
    @Deprecated
    public boolean sendFrame(ByteBuffer buffer, int width, int height, int yStride, int uvStride) {
        if (!isInitialized) {
            Log.e(TAG, "Cannot send frame: OMTSender not initialized");
            return false;
        }

        if (!buffer.isDirect()) {
            Log.e(TAG, "Buffer must be a direct ByteBuffer");
            return false;
        }

        return nativeSendFrame(buffer, width, height, yStride, uvStride);
    }

    /**
     * Bytes of NV12 a destination buffer must hold for a frame whose luma rowStride is
     * {@code yRowStride}. Same arithmetic as {@code omtpack::nv12Size} in
     * app/src/main/cpp/omtnv12.h — the packer declares the stride, the caller allocates for it.
     *
     * @return the size, or -1 if the geometry is not packable (non-positive, or too large for a
     *         direct ByteBuffer / the sender's int length field)
     */
    public static long nv12BufferSize(int yRowStride, int height) {
        if (yRowStride <= 0 || height <= 0) return -1;
        long s = yRowStride, h = height;
        long bytes = h * s + (h / 2) * s;
        if (bytes <= 0 || bytes > Integer.MAX_VALUE) return -1;
        return bytes;
    }

    /**
     * Send one Camera2 {@code ImageFormat.YUV_420_888} frame, whatever chroma layout its HAL chose.
     *
     * The three planes go over as the Image described them; the native side reads each one with its
     * OWN rowStride and pixelStride, interleaves Cb/Cr when the chroma is planar (pixelStride==1),
     * and hands the encoder a buffer whose length is exactly {@code stride*height*3/2} — the number
     * the encoder is documented to read. A device that reports NV12 (pixelStride==2) takes the
     * verbatim-copy path and sees the same bytes on the wire as before.
     *
     * @param dst        Direct ByteBuffer of at least {@link #nv12BufferSize} bytes, reused per frame
     * @param y/u/v      the plane buffers from {@code Image.Plane.getBuffer()}, position 0
     * @param *Size      the readable bytes of that plane ({@code getBuffer().limit()})
     * @param *RowStride / *PixelStride from the same {@code Image.Plane}
     * @return true if the frame was packed and at least one receiver took it
     *
     * @apiNote SYNCHRONOUS: this call packs, encodes and sends before it returns, so the caller pays
     *          for the encode. The camera path is {@link #queueFramePlanes}; nothing that runs on an
     *          {@link android.media.ImageReader} callback should call this.
     */
    public boolean sendFramePlanes(ByteBuffer dst,
                                   ByteBuffer y, int ySize, int yRowStride, int yPixelStride,
                                   ByteBuffer u, int uSize, int uRowStride, int uPixelStride,
                                   ByteBuffer v, int vSize, int vRowStride, int vPixelStride,
                                   int width, int height) {
        if (!isInitialized) {
            Log.e(TAG, "Cannot send frame: OMTSender not initialized");
            return false;
        }
        if (dst == null || y == null || u == null || v == null) {
            Log.e(TAG, "Cannot send frame: a plane buffer is null");
            return false;
        }
        if (!dst.isDirect() || !y.isDirect() || !u.isDirect() || !v.isDirect()) {
            Log.e(TAG, "Cannot send frame: plane buffers must all be direct ByteBuffers");
            return false;
        }
        if (y.position() != 0 || u.position() != 0 || v.position() != 0) {
            // The native side addresses the allocation base, so a positioned buffer would be read
            // from the wrong offset. Refuse it rather than encode the wrong pixels.
            Log.e(TAG, "Cannot send frame: plane buffers must be positioned at 0");
            return false;
        }
        if (dst.capacity() < nv12BufferSize(yRowStride, height)) {
            Log.e(TAG, "Cannot send frame: destination buffer " + dst.capacity()
                    + " is smaller than the packed NV12 size for stride=" + yRowStride
                    + " height=" + height);
            return false;
        }

        return nativeSendFramePlanes(dst, y, ySize, yRowStride, yPixelStride,
                                     u, uSize, uRowStride, uPixelStride,
                                     v, vSize, vRowStride, vPixelStride, width, height);
    }

    /**
     * Hand one Camera2 {@code ImageFormat.YUV_420_888} frame to the native encoder thread.
     *
     * This is the capture path. It does the plane-descriptor checks, copies the plane bytes into a
     * ring slot and returns — the pack, the encode and the socket write happen on a dedicated
     * encoder thread, in the order the frames arrived. An {@code ImageReader} callback that packs and
     * encodes inline caps the delivered frame rate at 1/(its own cost) no matter what the camera was
     * asked for, which is the "~14 fps delivered, 30 declared" report this method exists to fix.
     *
     * @param y/u/v      the plane buffers from {@code Image.Plane.getBuffer()}, position 0
     * @param *Size      the readable bytes of that plane ({@code getBuffer().limit()})
     * @param *RowStride / *PixelStride from the same {@code Image.Plane}
     * @return true if this frame was admitted to the ring; false if it was refused. A {@code true}
     *         here means "the encoder thread owns it now", not "a receiver got it" — see
     *         {@link #getFramesSent()} for that and {@link #getRingDrops()} for what never got there.
     */
    public boolean queueFramePlanes(ByteBuffer y, int ySize, int yRowStride, int yPixelStride,
                                    ByteBuffer u, int uSize, int uRowStride, int uPixelStride,
                                    ByteBuffer v, int vSize, int vRowStride, int vPixelStride,
                                    int width, int height) {
        if (!isInitialized) {
            Log.e(TAG, "Cannot queue frame: OMTSender not initialized");
            return false;
        }
        if (y == null || u == null || v == null) {
            Log.e(TAG, "Cannot queue frame: a plane buffer is null");
            return false;
        }
        if (!y.isDirect() || !u.isDirect() || !v.isDirect()) {
            Log.e(TAG, "Cannot queue frame: plane buffers must all be direct ByteBuffers");
            return false;
        }
        if (y.position() != 0 || u.position() != 0 || v.position() != 0) {
            // The native side addresses the allocation base, so a positioned buffer would be read
            // from the wrong offset. Refuse it rather than encode the wrong pixels.
            Log.e(TAG, "Cannot queue frame: plane buffers must be positioned at 0");
            return false;
        }
        if (nv12BufferSize(yRowStride, height) < 0) {
            Log.e(TAG, "Cannot queue frame: no packed NV12 size for stride=" + yRowStride
                    + " height=" + height);
            return false;
        }

        return nativeQueueFramePlanes(y, ySize, yRowStride, yPixelStride,
                                      u, uSize, uRowStride, uPixelStride,
                                      v, vSize, vRowStride, vPixelStride, width, height);
    }

    /**
     * Frames the native encoder ring evicted to keep the capture callback non-blocking, since the
     * sender was created.
     *
     * Delivered rate alone cannot tell "the camera is producing 30 fps and the encoder keeps up" from
     * "the camera is producing 30 fps and the ring throws most of them away"; this is the number that
     * separates them, and it belongs next to {@link #getFramesDropped()} (network drops) precisely so
     * the two stalls cannot be confused for each other.
     */
    public long getRingDrops() {
        return isInitialized ? nativeGetRingDrops() : 0;
    }

    /**
     * Get the number of currently connected receivers.
     */
    public int getConnectionCount() {
        return isInitialized ? nativeGetConnectionCount() : 0;
    }

    /**
     * Get the discovery address for this sender.
     * Format: "HOSTNAME (Name)"
     */
    public String getAddress() {
        return isInitialized ? nativeGetAddress() : null;
    }

    /**
     * Check if OMT sender is currently initialized and ready.
     */
    public boolean isReady() {
        return isInitialized;
    }

    /**
     * Cleanup and release resources.
     * Call this when streaming is stopped.
     */
    public void cleanup() {
        if (isInitialized) {
            nativeCleanup();
            isInitialized = false;
            Log.i(TAG, "OMT Sender cleaned up");
        }
    }

    /**
     * Set the current VMX quality level on the fly.
     * 
     * @param quality Video quality (0=Default, 1=Low, 50=Medium, 100=High)
     */
    public void setQuality(int quality) {
        if (isInitialized) {
            nativeSetQuality(quality);
        }
    }

    // =============================================================
    // Statistics API (for monitoring and UI feedback)
    // =============================================================

    /**
     * Get total frames sent since initialization.
     */
    public long getFramesSent() {
        return isInitialized ? nativeGetFramesSent() : 0;
    }

    /**
     * Get total frames dropped since initialization.
     * Frames are dropped when the receiver can't keep up (network congestion).
     */
    public long getFramesDropped() {
        return isInitialized ? nativeGetFramesDropped() : 0;
    }

    /**
     * Get recent frame drops and reset the counter.
     * Use this for periodic UI updates - call every second to get drops/second.
     * 
     * @return Number of frames dropped since last call
     */
    public long getRecentDropsAndReset() {
        return isInitialized ? nativeGetRecentDropsAndReset() : 0;
    }

    /**
     * Get total bytes sent since initialization.
     */
    public long getBytesSent() {
        return isInitialized ? nativeGetBytesSent() : 0;
    }

    /**
     * Get drop rate as percentage (0-100).
     * 
     * @return Drop rate percentage, or 0 if no frames sent
     */
    public float getDropRatePercent() {
        long sent = getFramesSent();
        long dropped = getFramesDropped();
        if (sent == 0)
            return 0f;
        return (dropped * 100f) / sent;
    }

    // Native method declarations
    private native boolean nativeInit(String name, int width, int height, int frameRateN, int frameRateD, int quality);

    private native boolean nativeSendFrame(ByteBuffer buffer, int width, int height, int yStride, int uvStride);

    private native boolean nativeSendFramePlanes(ByteBuffer dst,
                                                 ByteBuffer y, int ySize, int yRowStride, int yPixelStride,
                                                 ByteBuffer u, int uSize, int uRowStride, int uPixelStride,
                                                 ByteBuffer v, int vSize, int vRowStride, int vPixelStride,
                                                 int width, int height);

    private native boolean nativeQueueFramePlanes(ByteBuffer y, int ySize, int yRowStride, int yPixelStride,
                                                  ByteBuffer u, int uSize, int uRowStride, int uPixelStride,
                                                  ByteBuffer v, int vSize, int vRowStride, int vPixelStride,
                                                  int width, int height);

    private native long nativeGetRingDrops();

    private native int nativeGetConnectionCount();

    private native String nativeGetAddress();

    private native void nativeSetSenderInfo(String productName, String manufacturer);

    private native void nativeCleanup();

    // Statistics native methods
    private native long nativeGetFramesSent();

    private native long nativeGetFramesDropped();

    private native long nativeGetRecentDropsAndReset();

    private native long nativeGetBytesSent();

    private native void nativeSetQuality(int quality);

    private native int nativeGetProfile();

    /**
     * Get the current VMX profile ID.
     */
    public int getProfile() {
        return isInitialized ? nativeGetProfile() : 0;
    }
}
