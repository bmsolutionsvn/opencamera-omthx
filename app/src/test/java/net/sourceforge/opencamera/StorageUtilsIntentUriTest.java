package net.sourceforge.opencamera;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

/**
 * Attack tests for the EXTRA_OUTPUT confused-deputy guard (adopted from PR #8).
 *
 * A third-party app that launches ACTION_IMAGE_CAPTURE / ACTION_VIDEO_CAPTURE with
 * MediaStore.EXTRA_OUTPUT chooses where Open Camera writes - and the write happens
 * with Open Camera's own permissions (ImageSaver.java:3213 openOutputStream(saveUri),
 * Preview.java:6501 openFileDescriptor(uri, "rw")), so a file:// EXTRA_OUTPUT aimed
 * at shared_prefs/*.xml or databases/*.db inside our data directory would clobber
 * our private files with JPEG/MPEG bytes.
 *
 * These tests are not string-helper checks: each attack case plants a REAL file
 * with sentinel content and then performs the write the app would perform, gated
 * only by StorageUtils.isOutputPathSafe. If the guard is removed (or returns true
 * unconditionally) the sentinel is destroyed and the test goes red - that is the
 * RED-before-GREEN condition. The legitimate cases assert the opposite direction:
 * the guard must ALLOW and the write must land, so an over-broad guard (e.g. one
 * that blocks anything containing "data", or blocks everything outside MediaStore)
 * also goes red.
 */
public class StorageUtilsIntentUriTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static final String SENTINEL = "ORIGINAL-PREFS-BYTES";

    private File root;
    private File dataDir;      // stands in for /data/data/io.github.ysdede.opencameraomt
    private File victimPrefs;
    private File victimDb;
    private File externalDir;  // stands in for /storage/emulated/0

    @Before
    public void setUp() throws IOException {
        root = tmp.newFolder("device");
        dataDir = new File(root, "data/io.github.ysdede.opencameraomt");
        assertTrue(new File(dataDir, "shared_prefs").mkdirs());
        assertTrue(new File(dataDir, "databases").mkdirs());
        assertTrue(new File(dataDir, "cache").mkdirs());
        victimPrefs = new File(dataDir, "shared_prefs/io.github.ysdede.opencameraomt_preferences.xml");
        victimDb = new File(dataDir, "databases/mydb.sqlite");
        Files.write(victimPrefs.toPath(), SENTINEL.getBytes(StandardCharsets.UTF_8));
        Files.write(victimDb.toPath(), SENTINEL.getBytes(StandardCharsets.UTF_8));
        externalDir = new File(root, "storage/emulated/0");
        assertTrue(new File(externalDir, "DCIM/Camera").mkdirs());
    }

    /** What the app would do with an accepted EXTRA_OUTPUT: open the target for writing
     *  and stream capture data into it (the ContentResolver path for file:// URIs ends up
     *  at exactly this FileOutputStream-equivalent write with our uid's permissions).
     */
    private void attemptWrite(String path) throws IOException {
        try (FileOutputStream out = new FileOutputStream(path)) {
            out.write("MPEG-4-VIDEO-DATA".getBytes(StandardCharsets.UTF_8));
        }
    }

    /** Attack: the guard must refuse, and if the code obeyed the guard the victim must be intact.
     *  With the guard stubbed to always-allow, attemptWrite overwrites the sentinel and the
     *  final assertion fails - the test then reports the actual overwrite, not just a bool.
     */
    private void assertRefusedAndVictimIntact(String attackPath, File victim) throws IOException {
        boolean safe = StorageUtils.isOutputPathSafe(attackPath, dataDir.getAbsolutePath());
        if( safe )
            attemptWrite(attackPath); // mirrors the real flow: safe==true means the app writes
        assertEquals("private file was overwritten via EXTRA_OUTPUT-style path: " + attackPath,
                SENTINEL, new String(Files.readAllBytes(victim.toPath()), StandardCharsets.UTF_8));
        assertFalse("guard accepted a write into the private data dir: " + attackPath, safe);
    }

    /** Legitimate flow: guard must allow AND the write must land at the requested path. */
    private void assertAllowedAndWritable(String path) throws IOException {
        assertTrue("guard refused a legitimate output target: " + path,
                StorageUtils.isOutputPathSafe(path, dataDir.getAbsolutePath()));
        attemptWrite(path);
        assertTrue("allowed target was not actually writable: " + path, new File(path).exists());
    }

    // ---- the attacks ----

    /** Direct file:// EXTRA_OUTPUT at our preferences file. */
    @Test
    public void cannotOverwritePreferencesDirectly() throws IOException {
        assertRefusedAndVictimIntact(victimPrefs.getAbsolutePath(), victimPrefs);
    }

    /** Same target reached through '..' segments - canonical resolution must see through it. */
    @Test
    public void cannotOverwritePreferencesViaTraversal() throws IOException {
        File cacheSub = new File(dataDir, "cache/subdir");
        assertTrue(cacheSub.mkdirs());
        // from cache/subdir: one '..' -> cache, second '..' -> dataDir, then back down
        String traversal = cacheSub.getAbsolutePath() + "/../../shared_prefs/" + victimPrefs.getName();
        assertRefusedAndVictimIntact(traversal, victimPrefs);
    }

    /** Same target reached through a symlink planted outside the data dir - the naive
     *  startsWith check on the raw path would wave this through.
     */
    @Test
    public void cannotOverwritePreferencesViaSymlink() throws IOException {
        Path link = new File(root, "public_link").toPath();
        Files.createSymbolicLink(link, dataDir.toPath());
        assertRefusedAndVictimIntact(link + "/shared_prefs/" + victimPrefs.getName(), victimPrefs);
    }

    /** Database target, also via traversal from an unrelated public directory. */
    @Test
    public void cannotOverwriteDatabaseViaTraversalFromOutside() throws IOException {
        File publicTmp = new File(root, "storage/emulated/0/Download");
        assertTrue(publicTmp.mkdirs());
        // from Download climb four levels back to the device root (Download/0/emulated/storage),
        // then walk into the private tree
        String traversal = publicTmp.getAbsolutePath() + "/../../../../data/io.github.ysdede.opencameraomt/databases/mydb.sqlite";
        assertRefusedAndVictimIntact(traversal, victimDb);
    }

    /** The data directory itself is not a writable target either. */
    @Test
    public void dataDirItselfRefused() {
        assertFalse(StorageUtils.isOutputPathSafe(dataDir.getAbsolutePath(), dataDir.getAbsolutePath()));
    }

    // ---- the flows that must keep working ----

    /** Legacy third-party contract: file:// EXTRA_OUTPUT to shared external storage. */
    @Test
    public void externalStorageOutputStillWorks() throws IOException {
        assertAllowedAndWritable(new File(externalDir, "DCIM/Camera/VID_from_intent.mp4").getAbsolutePath());
    }

    /** A sibling of the data dir sharing its name as a string prefix must NOT be blocked -
     *  this is the substring-match bug (canonicalPath.startsWith(dataDir) without separator).
     */
    @Test
    public void siblingWithSharedNamePrefixNotBlocked() throws IOException {
        File sibling = new File(root, "data/io.github.ysdede.opencameraomt2/out");
        assertTrue(sibling.mkdirs());
        assertAllowedAndWritable(new File(sibling, "IMG_from_intent.jpg").getAbsolutePath());
    }

    /** A content:// URI whose provider exposes no _data column (FileProvider, MediaStore
     *  entries on scoped storage) resolves to a null path - the caller's provider owns the
     *  write, so we must allow it (this is the standard modern capture-intent contract).
     */
    @Test
    public void unresolvableContentUriPathIsAllowed() {
        assertTrue("null resolved path refused - share/save flows would break",
                StorageUtils.isOutputPathSafe(null, dataDir.getAbsolutePath()));
        assertTrue("null dataDir refused - a broken getApplicationInfo() must not brick capture",
                StorageUtils.isOutputPathSafe("/storage/emulated/0/DCIM/Camera/x.mp4", null));
    }

    /** Traversal that canonicalises OUT of the data dir is legitimate (caller's own tree):
     *  /data/data/pkg/../external/vid.mp4 resolves to /data/external/vid.mp4.
     */
    @Test
    public void traversalThatLeavesTheDataDirIsAllowed() throws IOException {
        File outside = new File(root, "data/external_target");
        assertTrue(outside.mkdirs());
        String path = new File(dataDir, "cache").getAbsolutePath() + "/../../external_target/vid.mp4";
        assertAllowedAndWritable(path);
    }
}
