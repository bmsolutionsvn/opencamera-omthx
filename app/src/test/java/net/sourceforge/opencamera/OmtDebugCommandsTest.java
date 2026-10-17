package net.sourceforge.opencamera;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

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

    @Test
    public void everyRecognisedCommandIsConsumable() {
        /*
         * The consume rule (MainActivity calls removeExtra when this is true) is what stops a
         * once-issued command replaying on every activity recreation — getIntent() survives
         * recreate(), so an unconsumed stop_stream silently stops a stream the user later starts by
         * hand. Anything parse() accepts MUST therefore be consumed: if these two ever disagree,
         * the replay bug is back. (removeExtra itself needs a real Intent and is not host-testable;
         * the emulator battery's S2 does a genuine recreation and proves the effect.)
         */
        for (String raw : new String[] { "start_stream", "stop_stream", "status",
                " START_STREAM \n", "Status" }) {
            assertNotNull("parse accepted " + raw, OmtDebugCommands.parse(raw));
            assertTrue("a command parse() accepts must be consumed: " + raw,
                    OmtDebugCommands.isConsumableCommand(raw));
        }
    }

    @Test
    public void nothingUnrecognisedIsConsumable() {
        // A normal launch must not have its extras touched: refusing here is what keeps
        // handleOmtDebugIntent() out of the way of every non-debug launch, and it is the mirror of
        // nullEmptyAndGarbageAreNull above. If an unrecognised value were "consumable", a stray
        // extra would be silently deleted from an Intent the app does not own.
        assertFalse(OmtDebugCommands.isConsumableCommand(null));
        assertFalse(OmtDebugCommands.isConsumableCommand(""));
        assertFalse(OmtDebugCommands.isConsumableCommand("   "));
        assertFalse(OmtDebugCommands.isConsumableCommand("start"));
        assertFalse(OmtDebugCommands.isConsumableCommand("stop-stream"));
        assertFalse(OmtDebugCommands.isConsumableCommand("restart"));
    }
}
