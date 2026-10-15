package net.sourceforge.opencamera;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import android.content.SharedPreferences;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Date;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;

/**
 * Attack tests for the preference-derived media filename prefix (adopted from PR #6).
 *
 * Threat model: the photo/video filename prefix is an ordinary preference
 * (PreferenceKeys.SavePhotoPrefixPreferenceKey / SaveVideoPrefixPreferenceKey),
 * writable by anything that can edit SharedPreferences - the settings EditText, a
 * restored settings backup (BACKUP_OC_ files), or a future remote-settings surface.
 * createMediaFilename() concatenated it straight into the filename, and the filename
 * is resolved against the save directory as a single string
 * (createOutputMediaFile: new File(mediaStorageDir.getPath() + File.separator + mediaFilename),
 * StorageUtils.java:784) and written with a plain FileOutputStream (ImageSaver.java:1761).
 *
 * These tests drive the real createMediaFilename() with a hostile prefix and then
 * attempt the real write against the real resolved File - the assertion is that the
 * app's private data directory is NOT overwritten, not merely that a helper returns
 * a clean string.
 */
public class StorageUtilsPrefixTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private File saveDir;      // stands in for the DCIM/Camera save folder
    private File dataDir;      // stands in for /data/data/<package> - the victim
    private File victimPrefs;  // a real file inside dataDir the attack targets
    private StorageUtils storageUtils;

    /** Minimal SharedPreferences backed by a map; only getString/getBoolean are exercised. */
    private static class FakePrefs implements SharedPreferences {
        private final Map<String, String> strings = new HashMap<>();

        void put(String key, String value) { strings.put(key, value); }

        @Override public Map<String, ?> getAll() { return strings; }
        @Override public String getString(String key, String defValue) {
            return strings.containsKey(key) ? strings.get(key) : defValue;
        }
        @Override public Set<String> getStringSet(String key, Set<String> defValues) { return defValues; }
        @Override public int getInt(String key, int defValue) { return defValue; }
        @Override public long getLong(String key, long defValue) { return defValue; }
        @Override public float getFloat(String key, float defValue) { return defValue; }
        @Override public boolean getBoolean(String key, boolean defValue) { return defValue; }
        @Override public boolean contains(String key) { return strings.containsKey(key); }
        @Override public SharedPreferences.Editor edit() { throw new UnsupportedOperationException(); }
        @Override public void registerOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener l) {}
        @Override public void unregisterOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener l) {}
    }

    @Before
    public void setUp() throws IOException {
        File root = tmp.newFolder("world");
        saveDir = new File(root, "storage/emulated/0/DCIM/Camera");
        assertTrue(saveDir.mkdirs());
        dataDir = new File(root, "data/io.github.ysdede.opencameraomt");
        File sharedPrefs = new File(dataDir, "shared_prefs");
        assertTrue(sharedPrefs.mkdirs());
        victimPrefs = new File(sharedPrefs, "io.github.ysdede.opencameraomt_preferences.xml");
        Files.write(victimPrefs.toPath(), "SENTINEL-PREFS".getBytes(StandardCharsets.UTF_8));
        // the constructor only stores its arguments (StorageUtils.java:72), nulls are fine for
        // the createMediaFilename overload under test, which takes SharedPreferences explicitly
        storageUtils = new StorageUtils(null, null);
    }

    /** Replicates exactly how createOutputMediaFile resolves the name (StorageUtils.java:784)
     *  and how ImageSaver writes it (ImageSaver.java:1761): FileOutputStream to the resolved File.
     */
    private File resolveAndWrite(String filename) throws IOException {
        File mediaFile = new File(saveDir.getPath() + File.separator + filename);
        try (FileOutputStream out = new FileOutputStream(mediaFile)) {
            out.write("JPEGDATA".getBytes(StandardCharsets.UTF_8));
        }
        return mediaFile;
    }

    private void assertContainedAndVictimIntact(String filename) throws IOException {
        // attempt the real write FIRST: on unfixed code the traversal filename resolves into
        // the private data tree and this call really creates the file there, so the failure
        // messages below prove the attack was live, not theoretical
        File written = resolveAndWrite(filename);
        String[] privateDirContents = new File(dataDir, "shared_prefs").list();
        assertEquals("the capture pipeline created files in the app's private shared_prefs: "
                        + java.util.Arrays.toString(privateDirContents),
                1, privateDirContents == null ? 0 : privateDirContents.length);
        assertFalse("filename must not contain '/': " + filename, filename.contains("/"));
        assertFalse("filename must not contain '\\': " + filename, filename.contains("\\"));
        String canonicalSave = saveDir.getCanonicalPath() + File.separator;
        assertTrue("resolved file escaped the save directory: " + written.getCanonicalPath(),
                written.getCanonicalPath().startsWith(canonicalSave));
        String victimContent = new String(Files.readAllBytes(victimPrefs.toPath()), StandardCharsets.UTF_8);
        assertEquals("app private prefs were overwritten by the capture pipeline", "SENTINEL-PREFS", victimContent);
    }

    private FakePrefs prefs(String photoPrefix, String videoPrefix) {
        FakePrefs p = new FakePrefs();
        if( photoPrefix != null ) p.put(PreferenceKeys.SavePhotoPrefixPreferenceKey, photoPrefix);
        if( videoPrefix != null ) p.put(PreferenceKeys.SaveVideoPrefixPreferenceKey, videoPrefix);
        return p;
    }

    /** THE attack: a photo prefix that traverses out of DCIM/Camera into the app's private
     *  data directory and targets the real preferences file. Before the fix the returned
     *  filename keeps the '../' sequences, the resolved File lands on victimPrefs, and the
     *  FileOutputStream overwrites the sentinel - this test is RED on the unfixed code.
     */
    @Test
    public void photoPrefixCannotEscapeSaveDirIntoPrivateData() throws IOException {
        // depth: saveDir is root/storage/emulated/0/DCIM/Camera - five '../' steps climb back
        // to root, then the prefix walks into the sibling data/ tree and onto the prefs file
        String traversal = "../../../../../data/io.github.ysdede.opencameraomt/shared_prefs/io.github.ysdede.opencameraomt_preferences.";
        String name = storageUtils.createMediaFilename(StorageUtils.MEDIA_TYPE_IMAGE, "", 0, "xml",
                new Date(0L), prefs(traversal, null));
        assertContainedAndVictimIntact(name);
    }

    /** Same attack through the video prefix key. */
    @Test
    public void videoPrefixCannotEscapeSaveDir() throws IOException {
        String traversal = "../../../../../data/io.github.ysdede.opencameraomt/shared_prefs/prefs.";
        String name = storageUtils.createMediaFilename(StorageUtils.MEDIA_TYPE_VIDEO, "", 0, "mp4",
                new Date(0L), prefs(null, traversal));
        assertContainedAndVictimIntact(name);
    }

    /** Windows-style separators must not smuggle a traversal either (MediaStore DISPLAY_NAME
     *  and downstream tools have historically disagreed about '\').
     */
    @Test
    public void backslashPrefixCannotEscapeSaveDir() throws IOException {
        String name = storageUtils.createMediaFilename(StorageUtils.MEDIA_TYPE_IMAGE, "", 0, ".jpg",
                new Date(0L), prefs("..\\..\\..\\evil_", null));
        assertContainedAndVictimIntact(name);
    }

    /** No over-sanitisation: legitimate prefixes survive verbatim, including spaces and
     *  dots; the default (absent preference) is the documented IMG_/VID_.
     */
    @Test
    public void legitimatePrefixesArePreserved() throws IOException {
        String name = storageUtils.createMediaFilename(StorageUtils.MEDIA_TYPE_IMAGE, "", 0, ".jpg",
                new Date(0L), prefs(null, null));
        assertTrue("default photo prefix lost: " + name, name.startsWith("IMG_"));
        assertTrue(name.endsWith(".jpg"));
        String custom = storageUtils.createMediaFilename(StorageUtils.MEDIA_TYPE_IMAGE, "", 0, ".jpg",
                new Date(0L), prefs("My Photos 2026-", null));
        assertTrue("legitimate prefix not preserved verbatim: " + custom, custom.startsWith("My Photos 2026-"));
        String vid = storageUtils.createMediaFilename(StorageUtils.MEDIA_TYPE_VIDEO, "", 0, ".mp4",
                new Date(0L), prefs(null, null));
        assertTrue("default video prefix lost: " + vid, vid.startsWith("VID_"));
    }

    /** A prefix that is pure traversal must still produce a writable file inside the save
     *  dir (not an empty name, not a crash): the timestamp+extension keep it unique.
     */
    @Test
    public void pureTraversalPrefixStillYieldsLocalFile() throws IOException {
        String name = storageUtils.createMediaFilename(StorageUtils.MEDIA_TYPE_IMAGE, "", 0, ".jpg",
                new Date(0L), prefs("../../", null));
        assertContainedAndVictimIntact(name);
        if( name.isEmpty() )
            fail("sanitisation collapsed the filename to empty - saves would collide");
    }
}
