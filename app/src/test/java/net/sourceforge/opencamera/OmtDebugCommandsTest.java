package net.sourceforge.opencamera;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;

/**
 * Host unit tests for OmtDebugCommands.parse — the command surface of the debug intent driver.
 * These run on the JVM (no device): the point of the class is that parsing has no Android deps.
 */
public class OmtDebugCommandsTest {

    @Test
    public void startStreamRecognised() {
        assertEquals(OmtDebugCommands.Command.START_STREAM, OmtDebugCommands.parse("start_stream"));
    }

    @Test
    public void stopStreamRecognised() {
        assertEquals(OmtDebugCommands.Command.STOP_STREAM, OmtDebugCommands.parse("stop_stream"));
    }

    @Test
    public void statusRecognised() {
        assertEquals(OmtDebugCommands.Command.STATUS, OmtDebugCommands.parse("status"));
    }

    @Test
    public void shellLossinessTolerated() {
        // adb quoting is lossy: tolerate case and surrounding whitespace...
        assertEquals(OmtDebugCommands.Command.START_STREAM, OmtDebugCommands.parse(" START_STREAM \n"));
        assertEquals(OmtDebugCommands.Command.STATUS, OmtDebugCommands.parse("Status"));
    }

    @Test
    public void nullEmptyAndGarbageAreNull() {
        // ...but never guess: anything else must NOT map to a command, or a typo'd shell
        // invocation would silently start streaming.
        assertNull(OmtDebugCommands.parse(null));
        assertNull(OmtDebugCommands.parse(""));
        assertNull(OmtDebugCommands.parse("   "));
        assertNull(OmtDebugCommands.parse("start"));
        assertNull(OmtDebugCommands.parse("start_streams"));
        assertNull(OmtDebugCommands.parse("stop-stream"));
        assertNull(OmtDebugCommands.parse("restart"));
    }

    @Test
    public void internalDashNotUnderscoreRejected() {
        // The wire constants use underscores; the dash form is the most likely typo through a
        // shell and must be refused, not accepted.
        assertNull(OmtDebugCommands.parse("start-stream"));
        assertNull(OmtDebugCommands.parse("stop-stream"));
    }
}
