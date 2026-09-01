package dev.cajeta.idea.profiler

/**
 * What to say after a frame click (spec §8.2).
 *
 * `ProfileNavigation.open` has always returned a Boolean and the panel dropped
 * it, so a click that resolved nowhere did nothing and said nothing — and a
 * click that resolved to a file WITHOUT a line opened at the top, which from
 * the reader's chair looks the same as nothing happening. Reported three
 * separate ways on 2026-08-31 ("does not have a click", "or clicking it does
 * not take me to code", "it clicks, just takes me to the package declaration"),
 * which is one defect wearing three faces: the code knew which case it was in
 * and discarded the answer.
 *
 * Empty string means the navigation was unremarkable — the frame's file and
 * line were both known and it went there. Anything else is worth a line in the
 * status bar.
 */
object NavigationOutcome {

    fun describe(
        frame: String,
        location: ProfileSourceLocation?,
        opened: Boolean,
        exact: Boolean,
        /** Whether stdlib source was mounted when the lookup ran. */
        stdlibMounted: Boolean = true,
    ): String = when {
        // The trace never carried one. Not the same as a file we could not
        // find, and saying "not found" would send the reader looking for a
        // file that was never named.
        location == null || location.fileName.isEmpty() ->
            "no source location recorded for $frame"

        // Stdlib source is a searched root now, so "not found" no longer implies
        // "it was a stdlib frame". When the mount is MISSING that is the whole
        // explanation and it is actionable, so it is said first.
        !opened && !stdlibMounted ->
            "could not find ${location.fileName} — stdlib source is not mounted, " +
            "so every cajeta.* frame will fail; check the compiler path in " +
            "Settings > Languages & Frameworks > Cajeta"

        !opened ->
            "could not find ${location.fileName} under the project or stdlib " +
            "source roots"

        // Opened, but the trace had no line: it went to the top of the file.
        // Unstated, this is indistinguishable from a click that did nothing,
        // which is precisely how it was reported.
        !exact ->
            "${location.fileName} — the trace recorded no line for $frame, " +
            "so this is the top of the file"

        else -> ""
    }
}
