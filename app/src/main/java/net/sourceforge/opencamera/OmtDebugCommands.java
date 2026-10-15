package net.sourceforge.opencamera;

/**
 * Parsing for the debug-only OMT intent driver (see MainActivity).
 *
 * Why this exists: emulator QA drives the app over adb. UI automation (uiautomator taps on the
 * OMT streaming button) proved fragile in practice — first-run dialogs swallow taps, the dialog's
 * own button opens Chrome and steals focus, and buttons stay GONE while the camera opens. The
 * reliable path is to start the activity with an explicit intent extra that invokes the SAME
 * code path the on-screen button invokes (clickedOmtStreaming), so the test drives behaviour,
 * not pixels.
 *
 * Security: MainActivity honours these extras ONLY when the installed APK carries
 * FLAG_DEBUGGABLE (i.e. debug builds). A release build logs and ignores the command outright.
 *
 * The parsing is deliberately free of Android types so it can be unit-tested on the host.
 */
public final class OmtDebugCommands {
    /** Intent extra carrying the command; value is one of the COMMAND_* strings below. */
    public static final String EXTRA_COMMAND = "omt_debug_command";

    public static final String COMMAND_START_STREAM = "start_stream";
    public static final String COMMAND_STOP_STREAM = "stop_stream";
    public static final String COMMAND_STATUS = "status";

    public enum Command { START_STREAM, STOP_STREAM, STATUS }

    /**
     * Map a raw extra to a Command, or null for anything unrecognised (null extra, empty,
     * whitespace, unknown value). Whitespace and case are tolerated because the value arrives
     * through a shell (adb) where quoting is lossy.
     */
    public static Command parse(String raw) {
        if (raw == null)
            return null;
        String v = raw.trim().toLowerCase();
        switch (v) {
            case COMMAND_START_STREAM: return Command.START_STREAM;
            case COMMAND_STOP_STREAM:  return Command.STOP_STREAM;
            case COMMAND_STATUS:       return Command.STATUS;
            default:                   return null;
        }
    }

    private OmtDebugCommands() {}
}
