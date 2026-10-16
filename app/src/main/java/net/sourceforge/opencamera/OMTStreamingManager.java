package net.sourceforge.opencamera;

import android.graphics.ImageFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.content.Context;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.net.wifi.WifiManager;
import android.os.PowerManager;
import android.view.Surface;

import java.nio.ByteBuffer;

/**
 * OMT Streaming Manager - Manages the OMT network streaming lifecycle.
 * 
 * This class handles:
 * - mDNS service announcement (phone immediately discoverable)
 * - Creating and managing an ImageReader for frame capture
 * - Initializing the OMT sender with proper settings
 * - Converting camera frames to the format needed by OMT
 * - Thread-safe frame processing
 * 
 * Usage:
 * 1. Create instance with callback
 * 2. Call startAnnouncing() to make device discoverable via mDNS
 * 3. Call startStreaming() when user wants to start video streaming
 * 4. Add getStreamingSurface() to camera capture session surfaces
 * 5. Call stopStreaming() when done
 */
public class OMTStreamingManager {
    private static final String TAG = "OMTStreamingManager";
    private static final boolean DEBUG_LOGS = false; // Set to true for debugging

    // Callback interface for streaming events
    public interface StreamingCallback {
        void onAnnouncingStarted();

        void onStreamingStarted();

        void onStreamingStopped();

        void onStreamingError(String error);

        void onConnectionCountChanged(int count);

        /**
         * Called when the VMX quality profile changes dynamically.
         */
        void onQualityChanged(int profile);

        /**
         * Called when frames are being dropped due to network congestion.
         * 
         * @param droppedCount Number of frames dropped in the last reporting period
         * @param totalDropped Total frames dropped since streaming started
         * @param totalSent    Total frames sent since streaming started
         */
        void onFramesDropped(long droppedCount, long totalDropped, long totalSent);
    }

    private final Context context;
    private final StreamingCallback callback;
    private OMTSender omtSender;
    private ImageReader imageReader;
    private HandlerThread backgroundThread;
    private Handler backgroundHandler;

    private boolean isAnnouncing = false; // mDNS is active, device is discoverable
    private boolean isStreaming = false; // Camera frames are being sent

    // Streaming parameters
    private int streamWidth;
    private int streamHeight;
    private int streamFps;
    private int streamQuality;
    private String streamName;

    // mDNS and Locks
    private NsdManager nsdManager;
    private NsdManager.RegistrationListener registrationListener;
    /**
     * Bumped by every registerService() attempt. An async NsdManager callback belongs to the
     * attempt that was alive when it was issued, not to whichever announcement happens to exist
     * when the callback finally lands.
     */
    private volatile long registrationGeneration = 0;
    private WifiManager.MulticastLock multicastLock;
    private WifiManager.WifiLock wifiLock;
    private PowerManager.WakeLock wakeLock;
    private static final String SERVICE_TYPE = "_omt._tcp";

    public OMTStreamingManager(Context context, StreamingCallback callback) {
        // getApplicationContext(), not the Activity: this object outlives it. The manager holds
        // PowerManager/NsdManager handles derived from this context and runs a 1s monitor loop on
        // a HandlerThread that only stops when isAnnouncing goes false — so retaining the Activity
        // here keeps its whole view hierarchy (largeHeap is on) reachable from a thread the GC can
        // never trim. That retained graph is the mechanism behind "the processes don't seem to
        // terminate properly" in issue #9. NB: the StreamingCallback handed in by MainActivity is
        // still an anonymous inner class over the Activity, so this alone does not fully unpin it.
        this.context = context.getApplicationContext();
        this.callback = callback;
    }

    /**
     * Start mDNS announcement only - makes device discoverable without streaming.
     * This allows viewers to see this device in their sender list immediately.
     * Call startStreaming() later to actually start sending video frames.
     * 
     * @param width   Video width (for announcement)
     * @param height  Video height (for announcement)
     * @param fps     Frame rate (for announcement)
     * @param quality Quality setting (1=Low, 50=Medium, 100=High)
     * @param name    Stream name visible in discovery
     * @return true if announcement started successfully
     */
    public synchronized boolean startAnnouncing(int width, int height, int fps, int quality, String name) {
        if (isAnnouncing) {
            Log.d(TAG, "Already announcing");
            return true;
        }

        this.streamWidth = width;
        this.streamHeight = height;
        this.streamFps = fps;
        this.streamQuality = quality;
        this.streamName = name;

        Log.i(TAG, "Starting OMT announcement: " + name + " (" + width + "x" + height + "@" + fps + "fps)");

        try {
            // Start background thread
            startBackgroundThread();

            // Initialize OMT sender (creates TCP server + mDNS announcement)
            omtSender = new OMTSender();
            if (!omtSender.init(name, width, height, fps, quality)) {
                Log.e(TAG, "Failed to initialize OMT sender for announcement");
                cleanupAnnouncement();
                if (callback != null) {
                    callback.onStreamingError("Failed to initialize OMT sender");
                }
                return false;
            }

            // Set device information for handshake
            omtSender.setSenderInfo(android.os.Build.MODEL, android.os.Build.MANUFACTURER);

            // Register mDNS service (with frequent announcements)
            if (!registerService(6400, name)) {
                // Was: registerService() swallowed every failure (its whole body sat in a try/catch
                // that only logged) and isAnnouncing was set true regardless. The user saw
                // "device is now discoverable" while nothing was advertised, and the guard at the
                // top of this method refuses to try again while isAnnouncing is set — so the only
                // way back was killing the process. That is the "sometimes OBS cannot detect the
                // source" report from the phone's side, and it is unrecoverable by design.
                Log.e(TAG, "mDNS registration failed - not announcing");
                teardownFailedAnnouncement();
                if (callback != null) {
                    callback.onStreamingError("mDNS registration failed - device is not discoverable");
                }
                return false;
            }

            isAnnouncing = true;
            Log.i(TAG, "OMT announcement started - device is now discoverable");

            // Start connection monitoring
            startConnectionMonitor();

            if (callback != null) {
                callback.onAnnouncingStarted();
            }

            return true;
        } catch (Exception e) {
            Log.e(TAG, "Failed to start announcement: " + e.getMessage());
            cleanupAnnouncement();
            if (callback != null) {
                callback.onStreamingError("Failed to start announcement: " + e.getMessage());
            }
            return false;
        }
    }

    /**
     * Check if device is currently announcing (discoverable).
     */
    public boolean isAnnouncing() {
        return isAnnouncing;
    }

    /**
     * Start OMT video streaming.
     * If already announcing, this adds ImageReader to start sending camera frames.
     * If not announcing, this starts both announcement and streaming.
     * 
     * @param width   Video width
     * @param height  Video height
     * @param fps     Frame rate
     * @param quality Quality setting (1=Low, 50=Medium, 100=High)
     * @param name    Stream name visible in discovery
     * @return true if streaming started successfully
     */
    public synchronized boolean startStreaming(int width, int height, int fps, int quality, String name) {
        if (isStreaming) {
            Log.w(TAG, "Already streaming");
            return true;
        }

        Log.i(TAG, "Starting OMT streaming: " + width + "x" + height + "@" + fps + "fps, quality=" + quality);

        // If not already announcing, start announcement first
        if (!isAnnouncing) {
            if (!startAnnouncing(width, height, fps, quality, name)) {
                return false;
            }
        }

        try {
            // Create ImageReader for frame capture
            imageReader = ImageReader.newInstance(
                    width, height,
                    ImageFormat.YUV_420_888,
                    3 // Max images in the queue — and the reason the callback must be cheap: the
                      // camera cannot recycle a buffer until onImageAvailable returns, so the cost
                      // of that callback is the delivered frame rate. It queues into the bridge's
                      // encoder ring (same depth, app/src/main/cpp/OMTBridge.cpp) and returns.
            );
            imageReader.setOnImageAvailableListener(imageListener, backgroundHandler);

            isStreaming = true;
            Log.i(TAG, "OMT streaming started - camera frames will be sent");

            if (callback != null) {
                callback.onStreamingStarted();
            }

            return true;

        } catch (Exception e) {
            Log.e(TAG, "Error starting streaming: " + e.getMessage(), e);
            cleanupStreaming();
            if (callback != null) {
                callback.onStreamingError("Error: " + e.getMessage());
            }
            return false;
        }
    }

    /**
     * Stop video streaming only (keep mDNS announcement active).
     */
    public synchronized void stopStreaming() {
        if (!isStreaming) {
            return;
        }

        Log.i(TAG, "Stopping OMT streaming (keeping announcement active)");
        isStreaming = false;
        cleanupStreaming();

        if (callback != null) {
            callback.onStreamingStopped();
        }
    }

    /**
     * Stop both streaming and announcement (full shutdown).
     */
    public synchronized void stopAll() {
        Log.i(TAG, "Stopping all OMT services");
        isStreaming = false;
        isAnnouncing = false;
        unregisterService();
        cleanup();

        if (callback != null) {
            callback.onStreamingStopped();
        }
    }

    /**
     * Get the Surface for the ImageReader.
     * This surface should be added to the camera capture session.
     * 
     * @return Surface for frame capture, or null if not streaming
     */
    public Surface getStreamingSurface() {
        return imageReader != null ? imageReader.getSurface() : null;
    }

    /**
     * Check if currently streaming.
     */
    public boolean isStreaming() {
        return isStreaming;
    }

    /**
     * Get the current number of connected receivers.
     */
    public int getConnectionCount() {
        return omtSender != null ? omtSender.getConnectionCount() : 0;
    }

    /**
     * Get the OMT address for discovery.
     */
    public String getAddress() {
        return omtSender != null ? omtSender.getAddress() : null;
    }

    /**
     * The native encoder ring's own accounting, for the debug `status` command, in this order:
     * admitted, delivered, dropped (ring evictions), refused (encoder-side losses). All four are
     * read straight from their own counters — admitted is NOT computed from the other three, because
     * arithmetic that agrees with itself is exactly how a frame lost on an uncounted path stays
     * hidden. Zero across the board when no sender exists.
     */
    public long[] getRingAccounting() {
        if (omtSender == null) {
            return new long[] { 0, 0, 0, 0 };
        }
        return new long[] { omtSender.getRingAdmitted(), omtSender.getRingDelivered(),
                omtSender.getRingDrops(), omtSender.getRingRefused() };
    }

    /**
     * Get the current VMX profile ID.
     */
    public int getProfile() {
        return omtSender != null ? omtSender.getProfile() : 0;
    }

    /**
     * Update the current VMX quality level on the fly.
     */
    public void setQuality(int quality) {
        if (omtSender != null) {
            omtSender.setQuality(quality);
        }
    }

    // ImageReader listener - processes each available frame
    private int frameCount = 0;
    private final ImageReader.OnImageAvailableListener imageListener = new ImageReader.OnImageAvailableListener() {
        @Override
        public void onImageAvailable(ImageReader reader) {
            frameCount++;
            if (DEBUG_LOGS && frameCount % 30 == 1) {
                Log.d(TAG, "onImageAvailable called, frame #" + frameCount + ", isStreaming=" + isStreaming);
            }

            if (!isStreaming || omtSender == null) {
                // Drain the queue to prevent backpressure
                Image image = acquireLatestQuietly(reader, "drain");
                if (image != null) {
                    image.close();
                }
                return;
            }

            Image image = acquireLatestQuietly(reader, "send");
            if (image == null) {
                // Either nothing was pending or the reader was closed underneath us — the second
                // case is already counted and logged by acquireLatestQuietly. Neither is a frame.
                if (DEBUG_LOGS) Log.d(TAG, "acquireLatestImage returned null");
                return;
            }

            try {
                sendFrame(image);
            } finally {
                image.close();
            }
        }
    };

    /** How many times a callback fired against an already-closed ImageReader. */
    private int acquireOnClosedReaderCount = 0;

    /**
     * acquireLatestImage(), for a reader that another thread may be closing right now.
     *
     * The old code called reader.acquireLatestImage() bare, twice, outside any try — the only
     * guard in the callback wrapped sendFrame(), not the acquire — and cleanupStreaming() closes
     * the reader from the main thread (stopStreaming()/stopAll())
     * while these callbacks dispatch on the OMTStreamingThread HandlerThread. A callback already
     * queued when stop arrived, or one already in flight, then threw IllegalStateException("Image
     * Reader is not prepared") from a HandlerThread with no catch anywhere above it: logcat showed
     * the F DEBUG tombstone and the whole camera app vanished mid-stream. To a user that is "the
     * camera crashed the second I pressed stop", and it reproduced on a cycle (see the emulator
     * battery's S13, 20 start/stop cycles). A refused late frame is a dropped frame; an uncaught
     * exception in a callback is a dead process.
     */
    private Image acquireLatestQuietly(ImageReader reader, String where) {
        try {
            return reader.acquireLatestImage();
        } catch (IllegalStateException e) {
            acquireOnClosedReaderCount++;
            if (acquireOnClosedReaderCount <= 3 || (DEBUG_LOGS && acquireOnClosedReaderCount % 30 == 0)) {
                Log.w(TAG, where + ": acquireLatestImage on an ImageReader that was already closed ("
                        + acquireOnClosedReaderCount + " time(s)) — a queued callback lost the race "
                        + "with cleanupStreaming(); the frame is dropped, the process stays up");
            }
            return null;
        }
    }

    private int sentFrameCount = 0;
    private int sendFailureCount = 0;

    /**
     * Send a frame to the OMT sender.
     *
     * YUV_420_888 is a description of three planes, not a memory layout: the HAL decides whether
     * the chroma is interleaved (NV12, planes[1].getPixelStride()==2, one CbCrCr... buffer whose row
     * is width bytes) or planar (I420, pixelStride()==1, separate Cb and Cr planes whose rows are
     * width/2 bytes). This method used to copy planes[1] verbatim behind the luma plane and call the
     * result NV12, which on a planar device — the emulator included, measured at 1920x1080 with
     * y rowStride=1920 and chroma rowStride=960, pixelStride=1 — produced a buffer the sender's own
     * gate refused on every frame:
     *
     *   E omt-sender: Sender::send: frame rejected: buffer too small for NV12 1920x1080 stride=1920:
     *   the encoder reads 3110400 bytes, dataLength is 2592000
     *
     * 2592000 is 1080*1920 + 540*960: the chroma plane charged at its own half-width row while the
     * encoder walks it at the luma stride. The planes are now handed to the native boundary as the
     * Image described them; it reads each one with its OWN rowStride/pixelStride, interleaves Cb/Cr
     * when the layout is planar, and reports the geometry it really wrote, so the length the sender
     * is told and the bytes the encoder reads agree. That happens on the bridge's encoder thread —
     * see OMTSender.queueFramePlanes, and the frame-ring comment in app/src/main/cpp/OMTBridge.cpp.
     */
    private void sendFrame(Image image) {
        if (omtSender == null || !isStreaming) {
            return;
        }

        Image.Plane[] planes = image.getPlanes();
        if (planes.length < 3) {
            Log.e(TAG, "sendFrame: YUV_420_888 image reported " + planes.length + " planes, refusing it");
            return;
        }
        Image.Plane yPlane = planes[0];
        Image.Plane uPlane = planes[1];
        Image.Plane vPlane = planes[2];

        int width = image.getWidth();
        int height = image.getHeight();
        int yRowStride = yPlane.getRowStride();
        long packedSize = OMTSender.nv12BufferSize(yRowStride, height);
        if (packedSize < 0) {
            Log.e(TAG, "sendFrame: cannot size a packed NV12 buffer for stride=" + yRowStride
                    + " height=" + height + ", refusing the frame");
            return;
        }

        sentFrameCount++;
        if (DEBUG_LOGS && sentFrameCount % 30 == 1) {
            Log.d(TAG, "sendFrame #" + sentFrameCount + ": " + width + "x" + height
                    + ", y row=" + yRowStride + "/" + yPlane.getPixelStride()
                    + ", u row=" + uPlane.getRowStride() + "/" + uPlane.getPixelStride()
                    + ", v row=" + vPlane.getRowStride() + "/" + vPlane.getPixelStride()
                    + ", packedSize=" + packedSize);
        }

        ByteBuffer yBuffer = yPlane.getBuffer();
        yBuffer.rewind();
        ByteBuffer uBuffer = uPlane.getBuffer();
        uBuffer.rewind();
        ByteBuffer vBuffer = vPlane.getBuffer();
        vBuffer.rewind();

        // The capture path queues; it does not encode. Everything this call used to do inline —
        // packNv12, omt_send, Sender::send, the codec's Push/Join over its slice threads — now runs
        // on the bridge's encoder thread, because this method is the ImageReader callback and that
        // callback's cost IS the delivered frame rate (maxImages=3, so the camera waits on it).
        // Measured on one rig: native x86_64 emulator build, 30.4/30.2 fps; the same build under
        // arm64 translation, 11-13 fps. Same camera, same declared rate, different callback.
        //
        // There is deliberately no ByteBuffer allocated here any more: the native ring slot owns the
        // packed destination, because one frameBuffer reused across frames would let frame N+1's pack
        // overwrite bytes frame N is still handing to the socket.
        boolean queued = omtSender.queueFramePlanes(
                yBuffer, yBuffer.limit(), yRowStride, yPlane.getPixelStride(),
                uBuffer, uBuffer.limit(), uPlane.getRowStride(), uPlane.getPixelStride(),
                vBuffer, vBuffer.limit(), vPlane.getRowStride(), vPlane.getPixelStride(),
                width, height);

        if (!queued) {
            // The native side logs why; this is the tally that keeps a device-wide refusal from
            // looking like a healthy stream that is simply idle.
            sendFailureCount++;
            if (sendFailureCount <= 3 || (DEBUG_LOGS && sendFailureCount % 30 == 0)) {
                Log.w(TAG, "sendFrame: frame " + sentFrameCount + " was refused at the native boundary "
                        + "(" + sendFailureCount + " refused so far)");
            }
        }
    }

    private void startBackgroundThread() {
        if (backgroundThread != null) {
            return;
        }
        backgroundThread = new HandlerThread("OMTStreamingThread");
        backgroundThread.start();
        backgroundHandler = new Handler(backgroundThread.getLooper());
    }

    private void stopBackgroundThread() {
        if (backgroundThread == null) {
            return;
        }
        backgroundThread.quitSafely();
        try {
            backgroundThread.join();
        } catch (InterruptedException e) {
            Log.e(TAG, "Interrupted while stopping background thread");
        }
        backgroundThread = null;
        backgroundHandler = null;
    }

    private void startConnectionMonitor() {
        if (backgroundHandler == null) {
            return;
        }

        backgroundHandler.post(new Runnable() {
            private int lastCount = -1;
            private int lastProfile = -1;
            private long lastRingDrops = 0;

            @Override
            public void run() {
                // Monitor connections while announcing (even if not streaming)
                if (!isAnnouncing || omtSender == null) {
                    return;
                }

                /*
                 * Reap the subscribers that are gone — every tick, whether or not frames are
                 * flowing.
                 *
                 * The only code that unlinks a dead channel is cleanupDisconnected(), and it runs
                 * from Sender::send() and Sender::getStats(). send() is dead the moment the camera
                 * frames stop, and until now so was the only getStats() path in the app: every
                 * native stats call sat inside the `if (isStreaming)` block below, so after
                 * stop_stream the monitor's remaining work was getConnectionCount()/getProfile(),
                 * which count and never reap. A viewer that hung up while the announcement sat idle
                 * therefore pinned its socket fd, its senderLoop thread and its AsyncPool for the
                 * life of the announcement — the half-close rule in OMTChannel::receiverLoop keeps
                 * a peer that closed only its write half marked connected on purpose, so nothing
                 * else ever notices it is gone. Twenty churns after a stop and the process is
                 * twenty fds and threads short of a camera it can still open.
                 *
                 * The interval is unchanged (1s); this is one extra native call per tick on the
                 * same thread, and it is what makes the idle announcement as hygienic as a live one.
                 */
                omtSender.pollStatsForReaping();

                // Check connection count
                int count = omtSender.getConnectionCount();
                if (count != lastCount) {
                    lastCount = count;
                    if (callback != null) {
                        callback.onConnectionCountChanged(count);
                    }
                }

                // Check for quality changes
                int currentProfile = omtSender.getProfile();
                if (currentProfile != lastProfile) {
                    if (lastProfile != -1) { // Only notify if it's a change, not the first check
                        if (callback != null) {
                            callback.onQualityChanged(currentProfile);
                        }
                    }
                    lastProfile = currentProfile;
                }

                // Check for frame drops (only while streaming)
                if (isStreaming) {
                    long recentDrops = omtSender.getRecentDropsAndReset();
                    // The encoder ring's own drops are a different stall from a network stall: the
                    // camera produced the frame and the encode could not take it in time. Report both
                    // — a delivered rate alone cannot tell them apart, and only one of them is Wi-Fi.
                    long ringDrops = omtSender.getRingDrops();
                    if (ringDrops > lastRingDrops) {
                        Log.w(TAG, "Encoder ring dropped " + (ringDrops - lastRingDrops)
                                + " frame(s) (total " + ringDrops + ") — the encode cannot keep up"
                                + " with " + streamFps + " fps at " + streamWidth + "x" + streamHeight);
                        lastRingDrops = ringDrops;
                    }
                    if (recentDrops > 0) {
                        long totalDropped = omtSender.getFramesDropped();
                        long totalSent = omtSender.getFramesSent();
                        Log.w(TAG, "Frame drops detected: " + recentDrops + " in last second (total: " +
                                totalDropped + "/" + totalSent + ", " +
                                String.format("%.1f%%", omtSender.getDropRatePercent()) + " drop rate)");
                        if (callback != null) {
                            callback.onFramesDropped(recentDrops, totalDropped, totalSent);
                        }
                    }
                }

                // Check again in 1 second
                if (backgroundHandler != null && isAnnouncing) {
                    backgroundHandler.postDelayed(this, 1000);
                }
            }
        });
    }

    /**
     * Cleanup streaming only (ImageReader), keep OMT sender and mDNS active.
     */
    private void cleanupStreaming() {
        // Release ImageReader only.
        //
        // Detach the listener BEFORE closing, and give each step its own try/catch — the same
        // discipline unregisterService() documents for the locks. Callbacks dispatch on the
        // OMTStreamingThread HandlerThread while this runs on the main thread, so closing first and
        // detaching second left a window where an already-queued callback saw a dead reader: that
        // is the IllegalStateException the acquire guard now absorbs, and the detach order is what
        // stops new work from arriving after the close. Nulling the field first also means
        // getStreamingSurface() can never hand out a Surface whose reader is gone.
        //
        // Neither call had any guard at all before, so whichever one threw first jumped the rest —
        // including the `imageReader = null` — and the manager kept serving a closed reader.
        final ImageReader reader = imageReader;
        if (reader != null) {
            imageReader = null;
            try {
                reader.setOnImageAvailableListener(null, null);
            } catch (Exception e) {
                Log.e(TAG, "Failed to detach the ImageReader listener", e);
            }
            try {
                reader.close();
            } catch (Exception e) {
                Log.e(TAG, "Failed to close the ImageReader", e);
            }
        }
        // The packed-frame buffers live in the native ring now, and the encoder thread that owns
        // them is stopped and joined by OMTSender.cleanup() -> nativeCleanup, which drains what the
        // camera already handed over before it lets the sender go away.
    }

    /**
     * Cleanup announcement (OMT sender, mDNS, locks).
     */
    private synchronized void cleanupAnnouncement() {
        // Cleanup OMT sender
        if (omtSender != null) {
            omtSender.cleanup();
            omtSender = null;
        }
        // Stop background thread
        stopBackgroundThread();
    }

    /**
     * Tear down an announcement that never became discoverable: the mDNS registration and the three
     * locks, then the sender and the monitor thread. This is the path a FAILED registration must
     * take, sync (registerService returned false, or threw) or async (onRegistrationFailed).
     *
     * unregisterService() is the only code that releases the Wi-Fi, wake and multicast locks, and it
     * is safe to call for a registration mdnsd refused: it gives every resource its own try/catch
     * and its own null-out, so the IllegalArgumentException NsdManager throws when the registration
     * callback never fired costs one log line and nothing else — the locks still come back. Skipping
     * it is what made an async registration failure a permanent leak: the PARTIAL_WAKE_LOCK is
     * acquired with no timeout, the MulticastLock is reference-counted (so the next attempt's
     * acquire() could never be matched by the single release()), and the sender keeps port 6400
     * bound against the retry.
     *
     * Synchronized, because it runs on whatever thread NsdManager delivers its callback on, while
     * start/stopStreaming hold the monitor on the main thread.
     */
    private synchronized void teardownFailedAnnouncement() {
        unregisterService();
        cleanupAnnouncement();
    }

    /**
     * Full cleanup - both streaming and announcement.
     */
    private void cleanup() {
        cleanupStreaming();
        cleanupAnnouncement();
    }

    // ===== Service Discovery =====

    /**
     * @return true only if the registration request was accepted by NsdManager. A false here means
     * the device is NOT discoverable; callers must not treat announcing as started.
     */
    private boolean registerService(int port, String serviceName) {
        try {
            // 1. Acquire Multicast Lock
            //
            // Every lock below is acquired IDEMPOTENTLY: reuse the object the field already holds
            // and only acquire() it when it is not already held. Overwriting the field is the leak —
            // the old code created and acquired a fresh MulticastLock on every registerService()
            // call, so a second announcement while the first was still held pushed that refcounted
            // lock to count 2 against an object the field no longer named, and the single release()
            // in unregisterService() could never take it back to 0. Same for the PARTIAL_WAKE_LOCK,
            // which is acquired with NO timeout at all: one leaked object and the CPU never sleeps
            // again for the life of the process.
            WifiManager wifiManager = (WifiManager) context.getApplicationContext()
                    .getSystemService(Context.WIFI_SERVICE);
            if (wifiManager != null) {
                if (multicastLock == null) {
                    multicastLock = wifiManager.createMulticastLock("OMT_mDNS");
                    multicastLock.setReferenceCounted(true);
                }
                if (!multicastLock.isHeld()) {
                    multicastLock.acquire();
                    Log.i(TAG, "Multicast lock acquired");
                } else {
                    Log.i(TAG, "Multicast lock already held — reusing it, not re-acquiring");
                }

                // 2. Acquire WiFi Lock
                if (wifiLock == null) {
                    if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.Q) {
                        wifiLock = wifiManager.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "OMT_Streaming_Lock");
                    } else {
                        wifiLock = wifiManager.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "OMT_Streaming_Lock");
                    }
                    wifiLock.setReferenceCounted(false);
                }
                if (!wifiLock.isHeld()) {
                    wifiLock.acquire();
                    Log.i(TAG, "High-Perf WiFi lock acquired");
                } else {
                    Log.i(TAG, "WiFi lock already held — reusing it, not re-acquiring");
                }
            }

            // 3. Acquire WakeLock
            PowerManager powerManager = (PowerManager) context.getSystemService(Context.POWER_SERVICE);
            if (powerManager != null) {
                if (wakeLock == null) {
                    wakeLock = powerManager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "OMT:StreamingWakeLock");
                }
                if (!wakeLock.isHeld()) {
                    wakeLock.acquire();
                    Log.i(TAG, "CPU WakeLock acquired");
                } else {
                    Log.i(TAG, "CPU WakeLock already held — reusing it, not re-acquiring");
                }
            }

            // 4. Register Service
            nsdManager = (NsdManager) context.getSystemService(Context.NSD_SERVICE);
            NsdServiceInfo serviceInfo = new NsdServiceInfo();
            serviceInfo.setServiceName(serviceName);
            serviceInfo.setServiceType(SERVICE_TYPE);
            serviceInfo.setPort(port);

            /*
             * Tag this attempt. onRegistrationFailed/onServiceRegistered can land long after the
             * announcement that caused them was replaced (stop, then a fresh start), and a stale
             * failure that tears down the CURRENT announcement takes down a working stream.
             */
            final long generation = ++registrationGeneration;

            registrationListener = new NsdManager.RegistrationListener() {
                @Override
                public void onServiceRegistered(NsdServiceInfo NsdServiceInfo) {
                    String mServiceName = NsdServiceInfo.getServiceName();
                    Log.i(TAG, "NSD Service registered: " + mServiceName);
                }

                @Override
                public void onRegistrationFailed(NsdServiceInfo serviceInfo, int errorCode) {
                    if (generation != registrationGeneration) {
                        Log.w(TAG, "Ignoring NSD registration failure (code " + errorCode + ") from a "
                                + "superseded announcement (gen " + generation + ", current "
                                + registrationGeneration + ") — tearing down on it would kill a live stream");
                        return;
                    }
                    // Async half of registration: NsdManager accepted the call and mdnsd refused it
                    // (duplicate name, malformed label, daemon trouble). Logging a code and moving on
                    // left the UI claiming "discoverable"; tell the user, and clear the flag so
                    // startAnnouncing() will actually try again instead of returning early forever.
                    Log.e(TAG, "NSD Registration failed: " + errorCode);
                    isAnnouncing = false;
                    /*
                     * And release what the synchronous half already took. registerService() returns
                     * true the moment NsdManager ACCEPTS, so by the time this callback fires the
                     * PARTIAL_WAKE_LOCK (acquired with no timeout), the WifiLock, the refcounted
                     * MulticastLock and the sender bound to port 6400 are all held. The old body
                     * cleared the flag and fired the error callback and nothing else: an operator
                     * saw "device is not discoverable" in the log while `dumpsys power` still
                     * listed OMT:StreamingWakeLock, the port stayed bound so the next start hit
                     * EADDRINUSE, and the retry acquired the refcounted multicast lock AGAIN
                     * against a lock object the field no longer pointed at — permanent, for the life
                     * of the process. An async failure is the same failure as a sync one and must
                     * go down the same path.
                     */
                    teardownFailedAnnouncement();
                    if (callback != null) {
                        callback.onStreamingError("mDNS registration failed (code " + errorCode +
                                ") - device is not discoverable");
                    }
                }

                @Override
                public void onServiceUnregistered(NsdServiceInfo arg0) {
                    Log.i(TAG, "NSD Service unregistered");
                }

                @Override
                public void onUnregistrationFailed(NsdServiceInfo serviceInfo, int errorCode) {
                    Log.e(TAG, "NSD Unregistration failed: " + errorCode);
                }
            };

            if (nsdManager != null) {
                nsdManager.registerService(serviceInfo, NsdManager.PROTOCOL_DNS_SD, registrationListener);
                Log.i(TAG, "NSD registration initiated");
                return true;
            }
            Log.e(TAG, "NsdManager unavailable - the device will not be discoverable");
        } catch (Exception e) {
            // Used to be the only report of a failed registration, and the caller set isAnnouncing
            // true anyway.
            Log.e(TAG, "Failed during service registration", e);
        }
        return false;
    }

    private void unregisterService() {
        // Each resource gets its own try/catch and its own null-out. They used to share a single
        // try block, and NsdManager.unregisterService() throws IllegalArgumentException when the
        // registration callback has not fired yet — exactly the "start, then immediately background
        // or kill" sequence from issue #9. That exception jumped over every remaining statement, so
        // the Wi-Fi, wake and multicast locks were never released nor nulled; the next
        // startAnnouncing() then overwrote those fields with fresh lock objects, leaving a
        // PARTIAL_WAKE_LOCK (acquired with no timeout) and a reference-counted MulticastLock held
        // for the rest of the process's life. One failure must not be allowed to hide the rest.
        if (nsdManager != null && registrationListener != null) {
            try {
                nsdManager.unregisterService(registrationListener);
            } catch (Exception e) {
                Log.e(TAG, "Failed to unregister NSD service", e);
            }
            registrationListener = null;
        }

        if (wifiLock != null) {
            try {
                if (wifiLock.isHeld()) wifiLock.release();
            } catch (Exception e) {
                Log.e(TAG, "Failed to release WiFi lock", e);
            }
            wifiLock = null;
            Log.i(TAG, "WiFi lock released");
        }

        if (wakeLock != null) {
            try {
                if (wakeLock.isHeld()) wakeLock.release();
            } catch (Exception e) {
                Log.e(TAG, "Failed to release WakeLock", e);
            }
            wakeLock = null;
            Log.i(TAG, "WakeLock released");
        }

        if (multicastLock != null) {
            try {
                if (multicastLock.isHeld()) multicastLock.release();
            } catch (Exception e) {
                Log.e(TAG, "Failed to release multicast lock", e);
            }
            multicastLock = null;
            Log.i(TAG, "Multicast lock released");
        }
    }
}
