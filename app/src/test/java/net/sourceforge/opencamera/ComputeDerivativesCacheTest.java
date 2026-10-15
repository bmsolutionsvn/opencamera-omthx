package net.sourceforge.opencamera;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotSame;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;
import static org.junit.Assume.assumeTrue;

import java.util.Arrays;
import java.util.Random;

import org.junit.Test;

/**
 * Tests for the per-thread scratch-buffer optimisation in
 * JavaImageFunctions.ComputeDerivativesFunction (adopted from PR #5).
 *
 * The optimisation must be output-preserving, so every test compares the new
 * extracted row-computation (computeDerivativesRow) and buffer-reuse policy
 * (getThreadCache) against a verbatim copy of the original implementation
 * (referenceDerivativesChunk, copied from the pre-change apply()) on real
 * image data - gradients, step edges and high-frequency noise packed so the
 * alpha channel carries the intensity, exactly as PanoramaProcessor's
 * greyscale input does.
 */
public class ComputeDerivativesCacheTest {
    /**
     * Verbatim copy of the original ComputeDerivativesFunction.apply() inner
     * computation (origin/master, before the PR #5 rework), operating on
     * freshly allocated arrays. Returns {Ix, Iy} outputs of length
     * this_width*this_height.
     */
    private static int [][] referenceDerivativesChunk(int [] bitmap_in_cache_pixels, int bitmap_in_cache_y,
            int off_x, int off_y, int this_width, int this_height, int width, int height) {
        int [] cache_bitmap_Ix = new int[this_width*this_height];
        int [] cache_bitmap_Iy = new int[this_width*this_height];

        for(int y=off_y,c=0;y<off_y+this_height;y++) {
            int y_rel_bitmap_in_cache = y-bitmap_in_cache_y;
            for(int x=off_x;x<off_x+this_width;x++,c++) {
                int Ix = 0, Iy = 0;
                if( x >= 1 && x < width-1 && y >= 1 && y < height-1 ) {
                    // use Sobel operator
                    int pixel1 = bitmap_in_cache_pixels[(y_rel_bitmap_in_cache-1)*width+(x)] >>> 24;
                    int pixel3 = bitmap_in_cache_pixels[(y_rel_bitmap_in_cache)*width+(x-1)] >>> 24;
                    int pixel5 = bitmap_in_cache_pixels[(y_rel_bitmap_in_cache)*width+(x+1)] >>> 24;
                    int pixel7 = bitmap_in_cache_pixels[(y_rel_bitmap_in_cache+1)*width+(x)] >>> 24;

                    int iIx = (pixel5 - pixel3)/2;
                    int iIy = (pixel7 - pixel1)/2;

                    iIx = Math.max(iIx, -127);
                    iIx = Math.min(iIx, 128);
                    iIx += 127;

                    iIy = Math.max(iIy, -127);
                    iIy = Math.min(iIy, 128);
                    iIy += 127;

                    Ix = iIx;
                    Iy = iIy;
                }
                cache_bitmap_Ix[c] = Ix << 24;
                cache_bitmap_Iy[c] = Iy << 24;
            }
        }
        return new int [][] { cache_bitmap_Ix, cache_bitmap_Iy };
    }

    /**
     * Builds a real-image-like ARGB array of width*height whose alpha channel
     * carries the greyscale intensity: horizontal gradient, vertical gradient,
     * a hard step edge and high-frequency noise, so the Sobel filter exercises
     * its clamp boundaries (-127/128) as well as the quiet paths.
     */
    private static int [] makeImageData(int width, int height, long seed) {
        Random r = new Random(seed);
        int [] pixels = new int[width*height];
        for(int y=0;y<height;y++) {
            for(int x=0;x<width;x++) {
                int v;
                if( y > height/3 && y < 2*height/3 && x > width/2 ) {
                    v = 255; // step edge interior
                } else if( r.nextInt(4) == 0 ) {
                    v = r.nextInt(256); // high-frequency noise
                } else {
                    v = (x*255/Math.max(1,width-1) + y*255/Math.max(1,height-1)) / 2; // gradients
                }
                v = Math.max(0, Math.min(255, v));
                pixels[y*width+x] = (v << 24) | (v << 16) | (v << 8) | v;
            }
        }
        return pixels;
    }

    /**
     * Simulates the FastAccessBitmap row windowing ApplyFunctionThread drives:
     * for each source row y the three-row window is served from a cache whose
     * first row is cache_y = min(max(0, y-4), height-cache_height), i.e. the
     * window slides exactly like FastAccessBitmap.cache(y-4) does.
     */
    private static void runNewChunk(int [] full_pixels, int width, int height, int cache_height,
            int off_x, int off_y, int this_width, int this_height,
            int [] out_Ix, int [] out_Iy) {
        int [] cache_pixels = new int[width*cache_height];
        for(int y=off_y,c=0;y<off_y+this_height;y++,c+=this_width) {
            int cache_y = Math.min(Math.max(0, y-1-4), height-cache_height); // window must cover y-1..y+1
            System.arraycopy(full_pixels, cache_y*width, cache_pixels, 0, width*cache_height);
            JavaImageFunctions.ComputeDerivativesFunction.computeDerivativesRow(
                    cache_pixels, cache_y, y, off_x, this_width, width, height, out_Ix, out_Iy, c);
        }
    }

    private static void runReferenceChunk(int [] full_pixels, int width, int height, int cache_height,
            int off_x, int off_y, int this_width, int this_height,
            int [] out_Ix, int [] out_Iy) {
        int [] cache_pixels = new int[width*cache_height];
        for(int y=off_y;y<off_y+this_height;y++) {
            int cache_y = Math.min(Math.max(0, y-1-4), height-cache_height);
            System.arraycopy(full_pixels, cache_y*width, cache_pixels, 0, width*cache_height);
            int [][] ref = referenceDerivativesChunk(cache_pixels, cache_y, off_x, y, this_width, 1, width, height);
            System.arraycopy(ref[0], 0, out_Ix, (y-off_y)*this_width, this_width);
            System.arraycopy(ref[1], 0, out_Iy, (y-off_y)*this_width, this_width);
        }
    }

    /** Bit-for-bit identical output vs the original implementation on real image data,
     *  across chunk layouts including the ragged final chunk ApplyFunctionThread produces.
     */
    @Test
    public void testDerivativesBitIdenticalToOriginal() {
        int [][] configs = { {8, 8}, {37, 19}, {64, 64}, {100, 65} }; // last one exercises chunk_size 64 + remainder
        for(int [] cfg : configs) {
            int width = cfg[0], height = cfg[1];
            if( height < 3 || width < 3 )
                continue;
            int [] pixels = makeImageData(width, height, width*1000L+height);
            int chunk = Math.min(64, height);
            int [] out_new_Ix = new int[width*height], out_new_Iy = new int[width*height];
            int [] out_ref_Ix = new int[width*height], out_ref_Iy = new int[width*height];
            int y = 0;
            while( y < height ) {
                int this_height = Math.min(chunk, height-y);
                runNewChunk(pixels, width, height, chunk, 0, y, width, this_height, out_new_Ix, out_new_Iy);
                runReferenceChunk(pixels, width, height, chunk, 0, y, width, this_height, out_ref_Ix, out_ref_Iy);
                y += this_height;
            }
            // anti-vacuity: the outputs must be real filter data - at least some elements
            // must carry an actual derivative value (not just the flat 127<<24 or border 0)
            int distinct = 0;
            for(int i=0;i<out_ref_Ix.length;i++)
                if( out_ref_Ix[i] != 0 && out_ref_Ix[i] != (127 << 24) ) distinct++;
            assertTrue(cfg[0]+"x"+cfg[1]+": reference produced no real derivative values - test data is vacuous", distinct > 0);
            assertArrayEquals("Ix mismatch for " + cfg[0]+"x"+cfg[1], out_ref_Ix, out_new_Ix);
            assertArrayEquals("Iy mismatch for " + cfg[0]+"x"+cfg[1], out_ref_Iy, out_new_Iy);
        }
    }

    /** The reuse policy: same-size or smaller requests must return the identical array
     *  (that is the allocation-churn elimination); a larger request must grow it.
     */
    @Test
    public void testThreadCacheReusePolicy() {
        int [][] caches = new int[4][];
        int [] first = JavaImageFunctions.ComputeDerivativesFunction.getThreadCache(caches, 0, 1000);
        assertSame("same-size request must not reallocate", first,
                JavaImageFunctions.ComputeDerivativesFunction.getThreadCache(caches, 0, 1000));
        assertSame("smaller request must not reallocate", first,
                JavaImageFunctions.ComputeDerivativesFunction.getThreadCache(caches, 0, 512));
        int [] grown = JavaImageFunctions.ComputeDerivativesFunction.getThreadCache(caches, 0, 2000);
        assertTrue("larger request must grow the buffer", grown.length >= 2000);
        // per-thread buffers are independent:
        int [] other = JavaImageFunctions.ComputeDerivativesFunction.getThreadCache(caches, 1, 1000);
        assertNotSame("thread 1 must not share thread 0's buffer", grown, other);
    }

    /** The stale-data hazard the reuse introduces: a big chunk followed by a smaller
     *  chunk on the same thread reuses an oversized buffer. Every element the smaller
     *  chunk publishes ([0, this_width*this_height)) must be freshly computed - poison
     *  from the earlier, larger pass must not survive into the setPixels read window.
     */
    @Test
    public void testNoStaleDataWhenReusingOversizedBuffer() {
        final int width = 100, height = 96, chunk = 64;
        int [] pixels = makeImageData(width, height, 4242);
        int [][] caches_Ix = new int[1][], caches_Iy = new int[1][];

        // pass 1: the large first chunk, leaving poison-worthy data behind
        int [] big_Ix = JavaImageFunctions.ComputeDerivativesFunction.getThreadCache(caches_Ix, 0, width*chunk);
        int [] big_Iy = JavaImageFunctions.ComputeDerivativesFunction.getThreadCache(caches_Iy, 0, width*chunk);
        Arrays.fill(big_Ix, 0xCAFEBABE);
        Arrays.fill(big_Iy, 0xCAFEBABE);
        runNewChunk(pixels, width, height, chunk, 0, 0, width, chunk, big_Ix, big_Iy);

        // pass 2: the ragged final chunk, 32 rows, MUST reuse the same oversized arrays
        int small_height = height-chunk;
        int [] small_Ix = JavaImageFunctions.ComputeDerivativesFunction.getThreadCache(caches_Ix, 0, width*small_height);
        int [] small_Iy = JavaImageFunctions.ComputeDerivativesFunction.getThreadCache(caches_Iy, 0, width*small_height);
        assertSame("smaller chunk must reuse the oversized buffer", big_Ix, small_Ix);
        // do NOT clear: this is the point where a broken reuse would leak pass-1 contents
        runNewChunk(pixels, width, height, chunk, 0, chunk, width, small_height, small_Ix, small_Iy);

        int [] ref_Ix = new int[width*small_height], ref_Iy = new int[width*small_height];
        runReferenceChunk(pixels, width, height, chunk, 0, chunk, width, small_height, ref_Ix, ref_Iy);
        // compare only the published read window - setPixels reads exactly [0, width*small_height)
        assertArrayEquals("stale data leaked into the smaller chunk's read window",
                ref_Ix, Arrays.copyOf(small_Ix, width*small_height));
        assertArrayEquals("stale data leaked into the smaller chunk's Iy read window",
                ref_Iy, Arrays.copyOf(small_Iy, width*small_height));
        // anti-vacuity: at least one element of the buffer beyond the read window still holds
        // pass-1 bytes - proving the buffer really was reused oversized rather than reallocated
        boolean poison_or_pass1_beyond = false;
        for(int i=width*small_height;i<small_Ix.length;i++)
            if( small_Ix[i] != 0 ) poison_or_pass1_beyond = true;
        assertTrue("read window was not exercised against a reused oversized buffer", poison_or_pass1_beyond);
    }

    /** Measured allocation-churn claim: with the cache, repeatedly fetching the scratch
     *  buffers for fixed-size chunks allocates (near) nothing; the original code allocated
     *  two this_width*this_height int arrays (2*100*64*4 = 51200 bytes) per chunk.
     *  This test is RED before the optimisation: getThreadCache did not exist, and the
     *  equivalent per-chunk cost was 51200 bytes, far above the 5120 byte/chunk ceiling.
     *  Uses reflection because the unit-test compile classpath is android.jar, which has
     *  no java.lang.management; the tests themselves run on a real JVM where the class exists.
     */
    @Test
    public void testAllocationChurnEliminated() throws Exception {
        Class<?> mf = Class.forName("java.lang.management.ManagementFactory");
        Object bean = mf.getMethod("getThreadMXBean").invoke(null);
        Class<?> itf = Class.forName("com.sun.management.ThreadMXBean");
        Boolean supported = (Boolean) itf.getMethod("isThreadAllocatedMemorySupported").invoke(bean);
        assumeTrue(supported != null && supported);
        java.lang.reflect.Method alloc = itf.getMethod("getThreadAllocatedBytes", long.class);
        long tid = Thread.currentThread().getId();

        final int width = 100, height = 64, chunk = 64;
        int [][] caches_Ix = new int[1][], caches_Iy = new int[1][];
        // warm-up: first call allocates the buffers themselves
        JavaImageFunctions.ComputeDerivativesFunction.getThreadCache(caches_Ix, 0, width*chunk);
        JavaImageFunctions.ComputeDerivativesFunction.getThreadCache(caches_Iy, 0, width*chunk);

        int iterations = 1000;
        long before = (Long) alloc.invoke(bean, tid);
        for(int i=0;i<iterations;i++) {
            JavaImageFunctions.ComputeDerivativesFunction.getThreadCache(caches_Ix, 0, width*chunk);
            JavaImageFunctions.ComputeDerivativesFunction.getThreadCache(caches_Iy, 0, width*chunk);
        }
        long after = (Long) alloc.invoke(bean, tid);
        double bytes_per_chunk = (double)(after-before)/iterations;
        System.out.println("MEASURED bytes_per_chunk=" + bytes_per_chunk);
        assertTrue("per-chunk allocation is " + bytes_per_chunk + " bytes; the original "
                + "implementation allocated 51200 - the cache is not eliminating churn",
                bytes_per_chunk < 5120.0); // <10% of the old cost
    }
}
