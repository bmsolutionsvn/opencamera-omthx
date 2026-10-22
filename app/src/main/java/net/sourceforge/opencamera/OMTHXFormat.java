package net.sourceforge.opencamera;

/**
 * The two decisions the OMT HX path makes before it touches MediaCodec: which geometry to ask the
 * encoder for, and how many bits to give it. Both are pure functions so the phone only has to prove
 * the wrapper around them works.
 *
 * Why not just use the video profile's size: the device decides. A phone may encode 4K60 but not
 * 4K120, or cap at 1080p for a given codec, and asking for what it cannot do fails the start with a
 * stack trace instead of a stream. The clamp here keeps the requested size and gives up frame rate
 * first (a softer picture is less noticeable than a smaller one), then steps down to a size the
 * codec does support. See docs/superpowers/specs/2026-10-22-omt-hx-sending-design.md 4.3.
 */
public final class OMTHXFormat {

    /** What the chosen encoder can do. Returning 0 means "this size is not supported at all". */
    public interface Limits {
        int maxFpsAt(int width, int height);
    }

    // Bits per pixel per frame for HEVC CBR. A 1080p30-shaped table would starve 4K120, so the
    // budget scales with pixels *and* frames; these three numbers are the only tuning knobs.
    private static final double BPP_LOW = 0.06;
    private static final double BPP_MEDIUM = 0.10;
    private static final double BPP_HIGH = 0.15;
    private static final double H264_FACTOR = 1.6;   // the same picture costs ~60% more in H.264
    private static final long MIN_BITRATE = 4_000_000L;
    private static final long MAX_BITRATE = 250_000_000L;

    private OMTHXFormat() {
    }

    /**
     * The largest geometry the encoder can honour, in the design's order:
     * the request itself; else the request at a lower rate; else the largest camera size (never
     * larger than the request) the codec supports at all, at its own rate.
     *
     * @param cameraSizes the camera's supported video sizes, as {width, height} pairs; may be null
     * @return {width, height, fps}, or null when the codec supports nothing in range
     */
    public static int[] choose(int reqW, int reqH, int reqFps, int[][] cameraSizes, Limits limits) {
        if (reqW <= 0 || reqH <= 0 || reqFps <= 0 || limits == null) {
            return null;
        }
        // 1. exactly what was asked for.
        int fps = limits.maxFpsAt(reqW, reqH);
        if (fps >= reqFps) {
            return new int[]{reqW, reqH, reqFps};
        }
        // 2. the requested size, at the highest rate it can do.
        if (fps > 0) {
            return new int[]{reqW, reqH, fps};
        }
        // 3. the largest camera size the codec can do, never above the request.
        int[] best = null;
        if (cameraSizes != null) {
            for (int[] size : cameraSizes) {
                if (size == null || size.length < 2 || size[0] <= 0 || size[1] <= 0) {
                    continue;
                }
                if (size[0] > reqW || size[1] > reqH) {
                    continue;
                }
                final int sizeFps = limits.maxFpsAt(size[0], size[1]);
                if (sizeFps <= 0) {
                    continue;
                }
                if (best == null || (long) size[0] * size[1] > (long) best[0] * best[1]) {
                    best = new int[]{size[0], size[1], Math.min(sizeFps, reqFps)};
                }
            }
        }
        return best;
    }

    /**
     * CBR bits per second for a stream of this geometry. {@code quality} is the existing OMT
     * preference value: 1 = Low, 50 = Medium, 100 = High, anything else (0 = Auto, which HX has no
     * receiver suggestion for) is treated as Medium.
     */
    public static int bitrate(int width, int height, int fps, int quality, boolean hevc) {
        final double bpp = quality == 1 ? BPP_LOW : quality == 100 ? BPP_HIGH : BPP_MEDIUM;
        final double bitsPerSecond = (double) width * (double) height * (double) fps * bpp
                * (hevc ? 1.0 : H264_FACTOR);
        long value = Math.round(bitsPerSecond);
        if (value < MIN_BITRATE) {
            value = MIN_BITRATE;
        }
        if (value > MAX_BITRATE) {
            value = MAX_BITRATE;
        }
        return (int) value;
    }
}