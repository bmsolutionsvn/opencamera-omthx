package net.sourceforge.opencamera;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.util.Log;
import android.view.Surface;

import java.io.IOException;
import java.nio.ByteBuffer;

/**
 * The H.265/H.264 hardware encoder behind OMT HX.
 *
 * Camera2 renders straight into {@link #getInputSurface()}; this class drains the encoded access
 * units and hands each one to a {@link Sink} with the OMT codec tag. Nothing here interprets the
 * bitstream - the receiver decodes it.
 *
 * Two details matter for a receiver that joins late or mid-GOP:
 *   - every IDR carries the SPS/PPS (KEY_PREPEND_HEADER_TO_SYNC_FRAMES), and a codec-config buffer
 *     handed out separately is held and prepended to the next keyframe;
 *   - the timestamp handed on is the buffer's presentation time in nanoseconds - the JNI converts
 *     it to the 100 ns ticks the OMT header carries.
 */
public class OMTHXEncoder {
    private static final String TAG = "OMTHXEncoder";
    private static final int IFRAME_INTERVAL_S = 1;
    private static final long DEQUEUE_TIMEOUT_US = 10_000;

    /** One encoded access unit, ready for {@code OMTSender.sendCoded}. */
    public interface Sink {
        void onAccessUnit(ByteBuffer data, int offset, int length, int codecTag,
                          int width, int height, int fpsN, int fpsD, long ptsNs, boolean keyframe);
    }

    private final MediaCodec codec;
    private final Surface surface;
    private final int codecTag;
    private final int width;
    private final int height;
    private final int fps;

    private volatile boolean running;
    private Thread thread;
    private Sink sink;
    private byte[] csd;
    private boolean released;

    /**
     * @param mime     {@code "video/hevc"} or {@code "video/avc"}, already checked against the
     *                 device's encoders by the caller
     * @param codecTag the OMT FourCC to stamp on every access unit (HEVC or H264)
     */
    public OMTHXEncoder(String mime, int codecTag, int width, int height, int fps, int bitrate)
            throws IOException {
        this.codecTag = codecTag;
        this.width = width;
        this.height = height;
        this.fps = fps;

        MediaFormat format = MediaFormat.createVideoFormat(mime, width, height);
        format.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        format.setInteger(MediaFormat.KEY_BIT_RATE, bitrate);
        format.setInteger(MediaFormat.KEY_BITRATE_MODE,
                MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR);
        format.setInteger(MediaFormat.KEY_FRAME_RATE, fps);
        format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, IFRAME_INTERVAL_S);
        // A receiver that connects after the stream started cannot decode anything before an IDR
        // that carries the parameter sets. One second is the ceiling on that black frame.
        format.setInteger(MediaFormat.KEY_PREPEND_HEADER_TO_SYNC_FRAMES, 1);
        format.setInteger(MediaFormat.KEY_OPERATING_RATE, fps);
        format.setInteger(MediaFormat.KEY_PRIORITY, 0);   // realtime
        if (MediaFormat.MIMETYPE_VIDEO_HEVC.equals(mime)) {
            // 8-bit 4:2:0 Main: the only shape the OMT HX receiver accepts (no 10-bit, no alpha).
            format.setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.HEVCProfileMain);
        }

        codec = MediaCodec.createEncoderByType(mime);
        codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        surface = codec.createInputSurface();
    }

    /** The surface to hand to the Camera2 session in HX mode. */
    public Surface getInputSurface() {
        return surface;
    }

    /** Starts the codec and the drain thread. */
    public void start(Sink sink) {
        this.sink = sink;
        codec.start();
        running = true;
        thread = new Thread(this::drainLoop, "OMTHXEncoderDrain");
        thread.setDaemon(true);
        thread.start();
    }

    /**
     * Stops the drain thread and releases the codec. Idempotent, and safe to call after the camera
     * has already let go of the surface (the caller sequences that: see Preview's CloseCameraCallback).
     */
    public void stop() {
        running = false;
        if (thread != null) {
            try {
                thread.join(1500);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            thread = null;
        }
        if (released) {
            return;
        }
        released = true;
        try {
            codec.stop();
        } catch (Exception e) {
            if (MyDebug.LOG) Log.d(TAG, "codec.stop after close: " + e);
        }
        try {
            codec.release();
        } catch (Exception e) {
            Log.w(TAG, "codec.release failed", e);
        }
        surface.release();
    }

    private void drainLoop() {
        final MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        while (running) {
            final int index;
            try {
                index = codec.dequeueOutputBuffer(info, DEQUEUE_TIMEOUT_US);
            } catch (IllegalStateException e) {
                if (running) {
                    Log.e(TAG, "dequeueOutputBuffer failed", e);
                }
                return;
            }
            if (index < 0) {
                // INFO_TRY_AGAIN_LATER / INFO_OUTPUT_FORMAT_CHANGED: the parameter sets arrive with
                // the keyframes (or as a codec-config buffer below), so there is nothing to publish.
                continue;
            }
            try {
                if (info.size > 0) {
                    ByteBuffer buffer = codec.getOutputBuffer(index);
                    if (buffer != null) {
                        buffer.position(info.offset);
                        buffer.limit(info.offset + info.size);
                        if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                            csd = new byte[info.size];
                            buffer.get(csd);
                        } else if (sink != null) {
                            emit(buffer, info);
                        }
                    }
                }
            } finally {
                codec.releaseOutputBuffer(index, false);
            }
            if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                return;
            }
        }
    }

    private void emit(ByteBuffer buffer, MediaCodec.BufferInfo info) {
        final boolean keyframe = (info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
        final long ptsNs = info.presentationTimeUs * 1000L;
        if (keyframe && csd != null && csd.length > 0) {
            // The encoder handed the parameter sets out of band: a receiver joining here needs them
            // in front of this IDR, exactly once.
            ByteBuffer withHeaders = ByteBuffer.allocateDirect(csd.length + info.size);
            withHeaders.put(csd);
            withHeaders.put(buffer);
            withHeaders.flip();
            sink.onAccessUnit(withHeaders, 0, withHeaders.limit(), codecTag,
                    width, height, fps, 1, ptsNs, true);
        } else {
            sink.onAccessUnit(buffer, info.offset, info.size, codecTag,
                    width, height, fps, 1, ptsNs, keyframe);
        }
    }
}