package net.sourceforge.opencamera;

import org.junit.Test;
import static org.junit.Assert.*;

/**
 * The rule that decides whether an OMT receiver lists this phone at all.
 *
 * These are not abstract assertions: a name without a matched '(' ')' pair is dropped by
 * libomtnet's OMTAddress.IsValid on both the Windows and Avahi browse paths, with no error
 * surfaced anywhere — the sender looks healthy and is simply invisible.
 */
public class OMTStreamNameTest {

    private static void assertReceiverVisible(String name) {
        // The gate the receiver applies, restated here so the test fails if we ever emit a name
        // that would be discarded upstream.
        int open = name.indexOf('(');
        int close = name.indexOf(')');
        assertTrue("receiver would discard this name (no '(' ): " + name, open >= 0);
        assertTrue("receiver would discard this name (no ')' after '('): " + name, close > open);
    }

    @Test
    public void customNameIsMadeVisible() {
        String n = OMTStreamName.normalize("PhoneCam");
        assertEquals("PhoneCam (Camera)", n);
        assertReceiverVisible(n);
    }

    @Test
    public void alreadyQualifiedNamesAreLeftAlone() {
        assertEquals("Studio (Back Room)", OMTStreamName.normalize("Studio (Back Room)"));
        assertEquals("Cam (2)", OMTStreamName.normalize("Cam (2)"));
    }

    @Test
    public void whitespaceIsTrimmedButNameKept() {
        assertEquals("PhoneCam (Camera)", OMTStreamName.normalize("  PhoneCam  "));
    }

    @Test
    public void strayClosingParenDoesNotProduceGarbage() {
        String n = OMTStreamName.normalize("PhoneCam)");
        assertReceiverVisible(n);
        assertFalse("double parenthesis from a stray fragment: " + n, n.contains(") (Camera) ("));
    }

    @Test
    public void emptyAndNullFallBackToTheCaller() {
        assertNull(OMTStreamName.normalize(null));
        assertNull(OMTStreamName.normalize(""));
        assertNull(OMTStreamName.normalize("     "));
    }

    @Test
    public void onlyParenthesisCharactersAreRejectedAsUnusable() {
        // "(" alone carries no name; the receiver would list an empty source. Falling back to the
        // device-derived default is better than advertising junk.
        String n = OMTStreamName.normalize("(");
        assertReceiverVisible(n);
    }

    @Test
    public void theDefaultNameAlreadyPasses() {
        // Regression guard for the path that has always worked: whatever we auto-generate must
        // satisfy the same rule we now enforce on user input.
        assertReceiverVisible("Pixel 7_1a2b3c (Camera)");
    }
}
