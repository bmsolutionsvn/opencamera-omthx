package net.sourceforge.opencamera;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.util.Arrays;
import java.util.Random;

/**
 * Tests for the precomputed equalisation LUT in AdjustHistogramApplyFunction
 * (adopted from PR #7).
 *
 * The PR replaces the per-pixel lazy getEqualValue() (four float divisions per
 * pixel) with a LUT built once per (tile, value) at construction. Two things
 * must hold:
 *   1. bit-for-bit identical output to the non-LUT path - pinned by
 *      testLutBitIdenticalToLazyPath against a verbatim copy of the original
 *      code, including the NaN/Inf (int)-cast behaviour of empty tiles;
 *   2. the LUT must not go stale - the prior review flagged that a cached LUT
 *      could survive histogram changes. The LUT is an instance final field
 *      built from the caller's fully-populated array (HDRProcessor.
 *      adjustHistogram never mutates c_histogram after construction), so the
 *      only ways this can regress are (a) the LUT becoming shared/static, or
 *      (b) a future caller mutating the histogram between construction and
 *      apply. testTwoInstancesDoNotShareLut kills (a); testHistogramIsFrozenAt
 *      Construction pins the documented freeze semantics and proves the
 *      mutation it protects against would change the result if read live.
 */
public class HistogramLutTest {

    /** Verbatim copy of the original getEqualValue() (origin/master, pre PR #7). */
    private static int referenceGetEqualValue(int [] c_histogram, int histogram_offset, int value) {
        int cdf_v = c_histogram[histogram_offset+value];
        int cdf_0 = c_histogram[histogram_offset];
        int n_pixels = c_histogram[histogram_offset+255];
        float num = (float)(cdf_v - cdf_0);
        float den = (float)(n_pixels - cdf_0);
        int equal_value = (int)( 255.0f * (num/den) ); // value that we should choose to fully equalise the histogram
        return equal_value;
    }

    /** Verbatim copy of the original AdjustHistogramApplyFunction.apply(CachedBitmap, int, int[], ...)
     *  from origin/master, with the instance fields passed as parameters and output written to pixels_out.
     */
    private static void referenceApply(float hdr_alpha, int n_tiles, int width, int height, int [] c_histogram,
            int [] pixels, int [] pixels_out, int off_x, int off_y, int this_width, int this_height) {
        for(int y=off_y,c=0;y<off_y+this_height;y++) {
            for(int x=off_x;x<off_x+this_width;x++,c++) {
                // this code is performance critical; note it's faster to avoid calls to Color.red/green/blue()
                int color = pixels[c];
                int r = (color >> 16) & 0xFF;
                int g = (color >> 8) & 0xFF;
                int b = color & 0xFF;

                int value = Math.max(r, g);
                value = Math.max(value, b);

                float tx = ((float)x*n_tiles)/(float)width - 0.5f;
                float ty = ((float)y*n_tiles)/(float)height - 0.5f;

                // inline floor for performance
                int ix = tx >= 0.0 ? (int)tx : (int)tx - 1;
                int iy = ty >= 0.0 ? (int)ty : (int)ty - 1;
                int equal_value;
                if( ix >= 0 && ix < n_tiles-1 && iy >= 0 && iy < n_tiles-1 ) {
                    int histogram_offset00 = 256*(ix*n_tiles+iy);
                    int histogram_offset10 = 256*((ix+1)*n_tiles+iy);
                    int histogram_offset01 = 256*(ix*n_tiles+iy+1);
                    int histogram_offset11 = 256*((ix+1)*n_tiles+iy+1);
                    int equal_value00 = referenceGetEqualValue(c_histogram, histogram_offset00, value);
                    int equal_value10 = referenceGetEqualValue(c_histogram, histogram_offset10, value);
                    int equal_value01 = referenceGetEqualValue(c_histogram, histogram_offset01, value);
                    int equal_value11 = referenceGetEqualValue(c_histogram, histogram_offset11, value);
                    float alpha = tx - ix;
                    float beta = ty - iy;

                    float equal_value0 = (1.0f-alpha)*equal_value00 + alpha*equal_value10;
                    float equal_value1 = (1.0f-alpha)*equal_value01 + alpha*equal_value11;
                    equal_value = (int)((1.0f-beta)*equal_value0 + beta*equal_value1);
                }
                else if( ix >= 0 && ix < n_tiles-1 ) {
                    int this_y = (iy<0) ? iy+1 : iy;
                    int histogram_offset0 = 256*(ix*n_tiles+this_y);
                    int histogram_offset1 = 256*((ix+1)*n_tiles+this_y);
                    int equal_value0 = referenceGetEqualValue(c_histogram, histogram_offset0, value);
                    int equal_value1 = referenceGetEqualValue(c_histogram, histogram_offset1, value);
                    float alpha = tx - ix;
                    equal_value = (int)((1.0f-alpha)*equal_value0 + alpha*equal_value1);
                }
                else if( iy >= 0 && iy < n_tiles-1 ) {
                    int this_x = (ix<0) ? ix+1 : ix;
                    int histogram_offset0 = 256*(this_x*n_tiles+iy);
                    int histogram_offset1 = 256*(this_x*n_tiles+iy+1);
                    int equal_value0 = referenceGetEqualValue(c_histogram, histogram_offset0, value);
                    int equal_value1 = referenceGetEqualValue(c_histogram, histogram_offset1, value);
                    float beta = ty - iy;
                    equal_value = (int)((1.0f-beta)*equal_value0 + beta*equal_value1);
                }
                else {
                    int this_x = (ix<0) ? ix+1 : ix;
                    int this_y = (iy<0) ? iy+1 : iy;
                    int histogram_offset = 256*(this_x*n_tiles+this_y);
                    equal_value = referenceGetEqualValue(c_histogram, histogram_offset, value);
                }

                int new_value = (int)( (1.0f-hdr_alpha) * value + hdr_alpha * equal_value );

                float scale = ((float)new_value) / (float)value;

                // need to add +0.5 so that we round to nearest
                r = Math.min(255, (int)(r * scale + 0.5f));
                g = Math.min(255, (int)(g * scale + 0.5f));
                b = Math.min(255, (int)(b * scale + 0.5f));
                // this code is performance critical; note it's faster to avoid calls to Color.argb()
                pixels_out[c] = (255 << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }

    /** Real-image-like random RGB data (seeded): a low-contrast cluster (so histogram
     *  equalisation genuinely moves pixels) plus pure black and white outliers.
     */
    private static int [] makePixels(int width, int height, long seed) {
        Random r = new Random(seed);
        int [] pixels = new int[width*height];
        for(int i=0;i<pixels.length;i++) {
            int rr = 60 + r.nextInt(90), gg = 70 + r.nextInt(80), bb = 50 + r.nextInt(100);
            if( i % 997 == 0 ) { rr = gg = bb = 0; }
            else if( i % 991 == 0 ) { rr = gg = bb = 255; }
            pixels[i] = (255 << 24) | (rr << 16) | (gg << 8) | bb;
        }
        return pixels;
    }

    /** Builds a realistic per-tile cumulative histogram of the max(r,g,b) value, the way
     *  HDRProcessor.adjustHistogram does (TYPE_VALUE), with optional degenerate tiles:
     *  every 4th tile left all-zero (empty tile -> 0/0 NaN path), every 7th saturated at
     *  value 0 (cdf_0 == n_pixels -> den==0 path).
     */
    private static int [] makeCumulativeHistogram(int [] pixels, int width, int height, int n_tiles, boolean degenerate) {
        int [] c_histogram = new int[n_tiles*n_tiles*256];
        for(int y=0;y<height;y++) {
            int j = Math.min(n_tiles-1, (int)(((float)y*n_tiles)/(float)height));
            for(int x=0;x<width;x++) {
                int i = Math.min(n_tiles-1, (int)(((float)x*n_tiles)/(float)width));
                int color = pixels[y*width+x];
                int r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
                int value = Math.max(r, Math.max(g, b));
                if( degenerate && j == 0 && (i % 4 == 0) )
                    continue; // empty tile
                c_histogram[256*(i*n_tiles+j)+value]++;
            }
        }
        for(int i=0;i<n_tiles*n_tiles;i++) {
            int off = 256*i;
            if( degenerate && i % 7 == 3 ) {
                // saturated: every pixel of this tile at value 0
                int total = 0;
                for(int v=0;v<256;v++) total += c_histogram[off+v];
                Arrays.fill(c_histogram, off, off+256, 0);
                c_histogram[off] = Math.max(1, total);
            }
            for(int v=1;v<256;v++)
                c_histogram[off+v] += c_histogram[off+v-1];
        }
        return c_histogram;
    }

    private static int [] runLutPath(float hdr_alpha, int n_tiles, int width, int height, int [] c_histogram,
            int [] pixels) {
        JavaImageFunctions.AdjustHistogramApplyFunction function =
                new JavaImageFunctions.AdjustHistogramApplyFunction(hdr_alpha, n_tiles, width, height, c_histogram);
        function.init(1);
        int [] out = new int[width*height];
        int chunk = 64;
        for(int y=0;y<height;y+=chunk) {
            int this_height = Math.min(chunk, height-y);
            int [] chunk_pixels = Arrays.copyOfRange(pixels, y*width, (y+this_height)*width);
            JavaImageProcessing.CachedBitmap output = new JavaImageProcessing.CachedBitmap(null, width, this_height);
            function.apply(output, 0, chunk_pixels, 0, y, width, this_height);
            System.arraycopy(output.getCachedPixelsI(), 0, out, y*width, width*this_height);
        }
        return out;
    }

    private static int [] runReference(float hdr_alpha, int n_tiles, int width, int height, int [] c_histogram,
            int [] pixels) {
        int [] out = new int[width*height];
        referenceApply(hdr_alpha, n_tiles, width, height, c_histogram, pixels, out, 0, 0, width, height);
        return out;
    }

    /** Bit-for-bit equality of the LUT path vs the verbatim non-LUT path on real image
     *  data, across tile counts, odd sizes (exercises the clamped edge branches), alpha
     *  values, and degenerate histograms (empty tiles -> NaN, saturated tiles -> den==0).
     */
    @Test
    public void testLutBitIdenticalToLazyPath() {
        int [][] configs = { {1, 32, 32}, {4, 64, 48}, {8, 97, 33}, {4, 64, 130} };
        float [] alphas = { 0.0f, 0.5f, 1.0f };
        for(int [] cfg : configs) {
            int n_tiles = cfg[0], width = cfg[1], height = cfg[2];
            int [] pixels = makePixels(width, height, 31337L*width+height);
            for(boolean degenerate : new boolean[]{false, true}) {
                int [] c_histogram = makeCumulativeHistogram(pixels, width, height, n_tiles, degenerate);
                for(float alpha : alphas) {
                    int [] got = runLutPath(alpha, n_tiles, width, height, c_histogram, pixels);
                    int [] want = runReference(alpha, n_tiles, width, height, c_histogram, pixels);
                    // anti-vacuity: at full alpha the equaliser must actually move pixels,
                    // otherwise both paths could agree on an untouched copy
                    if( alpha == 1.0f ) {
                        int moved = 0;
                        for(int i=0;i<got.length;i++)
                            if( got[i] != (0xFF000000 | pixels[i] & 0xFFFFFF) ) moved++;
                        assertTrue("n_tiles="+n_tiles+" degenerate="+degenerate+": histogram had no effect - vacuous comparison",
                                moved > got.length/10);
                    }
                    assertArrayEquals("n_tiles="+n_tiles+" size="+width+"x"+height+" alpha="+alpha+" degenerate="+degenerate,
                            want, got);
                }
            }
        }
    }

    /** Staleness guard (a): two instances with different histograms, interleaved - a static
     *  or otherwise shared LUT would make the second construction clobber the first one's
     *  lookups, and each interleaved apply would then disagree with its own reference.
     */
    @Test
    public void testTwoInstancesDoNotShareLut() {
        final int width = 64, height = 48, n_tiles = 4;
        int [] pixels = makePixels(width, height, 4242L);
        int [] histA = makeCumulativeHistogram(pixels, width, height, n_tiles, false);
        int [] histB = histA.clone();
        // distort B so its equalisation differs measurably from A
        for(int i=0;i<n_tiles*n_tiles;i++)
            for(int v=1;v<256;v++)
                histB[256*i+v] = Math.min(histB[256*i+v] + (v*7) % 13, histB[256*i+255]);
        JavaImageFunctions.AdjustHistogramApplyFunction fA =
                new JavaImageFunctions.AdjustHistogramApplyFunction(1.0f, n_tiles, width, height, histA);
        JavaImageFunctions.AdjustHistogramApplyFunction fB =
                new JavaImageFunctions.AdjustHistogramApplyFunction(1.0f, n_tiles, width, height, histB);
        fA.init(1);
        fB.init(1);
        int [] outA_ref = runReference(1.0f, n_tiles, width, height, histA, pixels);
        int [] outB_ref = runReference(1.0f, n_tiles, width, height, histB, pixels);
        assertFalse("histograms A and B produced identical equalisation - test data is vacuous",
                Arrays.equals(outA_ref, outB_ref));
        // interleave: A, B, A, B - a shared LUT makes the later A applies read B's table
        for(int round=0;round<2;round++) {
            assertArrayEquals("round "+round+": instance A drifted (shared LUT?)", outA_ref, runInstances(fA, pixels, width, height));
            assertArrayEquals("round "+round+": instance B drifted (shared LUT?)", outB_ref, runInstances(fB, pixels, width, height));
        }
    }

    private static int [] runInstances(JavaImageFunctions.AdjustHistogramApplyFunction function, int [] pixels,
            int width, int height) {
        int [] out = new int[width*height];
        JavaImageProcessing.CachedBitmap output = new JavaImageProcessing.CachedBitmap(null, width, height);
        function.apply(output, 0, pixels, 0, 0, width, height);
        System.arraycopy(output.getCachedPixelsI(), 0, out, 0, out.length);
        return out;
    }

    /** Staleness guard (b): the histogram is FROZEN at construction. Mutating the caller's
     *  array afterwards must not change the output (that is the documented semantics the
     *  sole call site satisfies), and - anti-vacuity - the same mutation must demonstrably
     *  change the output if it were read live, so this test would go red if someone
     *  re-introduced lazy reads of a mutated histogram without updating the contract.
     */
    @Test
    public void testHistogramIsFrozenAtConstruction() {
        final int width = 64, height = 48, n_tiles = 4;
        int [] pixels = makePixels(width, height, 999L);
        int [] hist = makeCumulativeHistogram(pixels, width, height, n_tiles, false);
        int [] histFrozen = hist.clone();
        JavaImageFunctions.AdjustHistogramApplyFunction function =
                new JavaImageFunctions.AdjustHistogramApplyFunction(1.0f, n_tiles, width, height, hist);
        // now mutate the caller's array the way a stale-cache bug would trip over:
        // inflate every tile's totals so the equalisation curve changes
        for(int i=0;i<n_tiles*n_tiles;i++)
            hist[256*i+255] *= 2;
        function.init(1);
        int [] got = runInstances(function, pixels, width, height);
        assertArrayEquals("output drifted after post-construction histogram mutation - LUT no longer frozen",
                runReference(1.0f, n_tiles, width, height, histFrozen, pixels), got);
        assertFalse("mutating the histogram would not have changed a live read - mutation is vacuous",
                Arrays.equals(got, runReference(1.0f, n_tiles, width, height, hist, pixels)));
    }
}
