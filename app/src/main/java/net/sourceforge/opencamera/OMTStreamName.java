package net.sourceforge.opencamera;

/**
 * The DNS-SD instance name an OMT receiver is willing to look at.
 *
 * Kept free of Android imports on purpose: this is the rule that decides whether a sender is
 * visible at all, and it can only be tested if it does not need an Activity, a SharedPreferences
 * or a Context to reach it.
 *
 * Why it exists: conforming OMT receivers (libomtnet's OMTAddress.IsValid, which both the Windows
 * DNS-SD browser and the Avahi one call before returning a result) silently discard any instance
 * name that does not contain a '(' and a ')'. Nothing else complains — the phone is up, the TCP
 * server accepts, the record is published — the receiver simply never lists it. A user who typed a
 * friendly name like "PhoneCam" into Settings > OMT Stream Name therefore produced a stream that
 * OBS could not see, while the default name (device model + " (Camera)") worked. That asymmetry is
 * the whole of "sometimes OBS cannot detect the source" for anyone who renamed the stream.
 */
public final class OMTStreamName {
    /** What the auto-generated name uses; also what we append to a user name that lacks it. */
    public static final String DEFAULT_SUFFIX = " (Camera)";

    private OMTStreamName() {}

    /**
     * Returns a name that will survive the receiver's IsValid() gate, or null if there is nothing
     * to normalise (the caller then falls back to its own device-derived default).
     */
    public static String normalize(String raw) {
        if (raw == null) return null;
        String name = raw.trim();
        if (name.isEmpty()) return null;

        // IsValid() wants both parentheses, opening before closing. A name the user chose to
        // already carry a qualifier — "Studio (Back Room)", "Cam (2)" — is left exactly as typed:
        // rewriting a name somebody picked for their own studio is not our business, and the
        // receiver only needs the pair to exist.
        int open = name.indexOf('(');
        int close = name.indexOf(')');
        if (open >= 0 && close > open) return name;

        // Strip a stray trailing fragment so we do not produce "PhoneCam) (Camera)" from
        // "PhoneCam)" — the malformed case the user is most likely to type by accident.
        String base = name;
        if (open < 0 && close >= 0) base = name.substring(0, close).trim();
        if (base.isEmpty()) base = name;
        return base + DEFAULT_SUFFIX;
    }
}
