package net.sourceforge.opencamera;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;

import org.junit.Test;

/**
 * The decisions the Hx path makes before it touches MediaCodec: which geometry to ask for, and how
 * many bits to give it. Both are pure so the device only has to prove the wrapper works.
 *
 * The clamp order is the design's (docs/superpowers/specs/2026-10-22-omt-hx-sending-design.md 4.3):
 * keep the size the video profile asked for and give up frame rate first, because a smaller picture
 * is a visible change to the operator while a lower rate is not.
 */
public class OMTHXFormatTest {

    /** A codec's capabilities, as a table of exact (w, h) -> max fps. Anything absent is unsupported. */
    private static OMTHXFormat.Limits limits(final int[][] table) {
        return new OMTHXFormat.Limits() {
            @Override
            public int maxFpsAt(int width, int height) {
                for (int[] row : table) {
                    if (row[0] == width && row[1] == height) {
                        return row[2];
                    }
                }
                return 0;
            }
        };
    }

    private static final int[][] CAMERA_4K = {{3840, 2160}, {1920, 1080}, {1280, 720}};

    @Test
    public void chooseKeepsTheRequestWhenItIsSupported() {
        int[][] codec = {{3840, 2160, 120}, {1920, 1080, 240}};
        assertArrayEquals(new int[]{3840, 2160, 120},
                OMTHXFormat.choose(3840, 2160, 120, CAMERA_4K, limits(codec)));
    }

    @Test
    public void chooseLowersTheRateNotTheSize() {
        int[][] codec = {{3840, 2160, 60}, {1920, 1080, 240}};
        assertArrayEquals(new int[]{3840, 2160, 60},
                OMTHXFormat.choose(3840, 2160, 120, CAMERA_4K, limits(codec)));
    }

    @Test
    public void chooseNeverExceedsTheRequestedRate() {
        int[][] codec = {{1920, 1080, 240}};
        assertArrayEquals(new int[]{1920, 1080, 120},
                OMTHXFormat.choose(1920, 1080, 120, CAMERA_4K, limits(codec)));
    }

    @Test
    public void chooseStepsDownToASizeTheCodecSupports() {
        int[][] codec = {{1920, 1080, 60}, {1280, 720, 60}};
        assertArrayEquals(new int[]{1920, 1080, 60},
                OMTHXFormat.choose(3840, 2160, 60, CAMERA_4K, limits(codec)));
    }

    @Test
    public void chooseTakesTheLargestSizeAtItsOwnRateWhenNothingReachesTheRequest() {
        int[][] codec = {{1920, 1080, 30}, {1280, 720, 60}};
        assertArrayEquals(new int[]{1920, 1080, 30},
                OMTHXFormat.choose(3840, 2160, 60, CAMERA_4K, limits(codec)));
    }

    @Test
    public void chooseRefusesWhatTheCodecCannotDoAtAll() {
        int[][] codec = {{640, 480, 0}};   // every size reports 0 fps: unsupported
        assertNull(OMTHXFormat.choose(1920, 1080, 60, CAMERA_4K, limits(codec)));
    }

    @Test
    public void bitrateScalesWithPixelsAndFrames() {
        assertEquals(4_000_000, OMTHXFormat.bitrate(1920, 1080, 30, 1, true));    // Low, raised to the floor
        assertEquals(7_464_960, OMTHXFormat.bitrate(1920, 1080, 60, 1, true));    // Low at 60 fps
        assertEquals(6_220_800, OMTHXFormat.bitrate(1920, 1080, 30, 50, true));   // Medium
        assertEquals(9_331_200, OMTHXFormat.bitrate(1920, 1080, 30, 100, true));  // High
    }

    @Test
    public void bitrateCovers4K120() {
        assertEquals(149_299_200, OMTHXFormat.bitrate(3840, 2160, 120, 100, true));
        assertEquals(238_878_720, OMTHXFormat.bitrate(3840, 2160, 120, 100, false)); // H.264 +60%
    }

    @Test
    public void bitrateIsClampedAtBothEnds() {
        assertEquals(250_000_000, OMTHXFormat.bitrate(7680, 4320, 60, 100, false));
        assertEquals(4_000_000, OMTHXFormat.bitrate(320, 240, 15, 1, true));
    }

    @Test
    public void bitrateTreatsAutoQualityAsMedium() {
        // The OMT preference uses 0 for "Auto (Receiver Suggested)"; Hx ignores suggestions, so Auto
        // has to mean one of the three real presets rather than a fourth, unknown bpp.
        assertEquals(OMTHXFormat.bitrate(1920, 1080, 30, 50, true),
                OMTHXFormat.bitrate(1920, 1080, 30, 0, true));
    }
}