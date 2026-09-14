package dev.cajeta.idea.xref

/**
 * lint-own-archive-classpath Unit 4 — whether the project's own archive is
 * usable, and what to tell the developer when it is not.
 *
 * This exists because the degraded state is INDISTINGUISHABLE from the bug the
 * rest of this spec fixes. Without the own archive a project's own types do not
 * resolve, so an unbuilt project shows exactly the red underlines the fix was
 * supposed to remove. Reporting the state is what separates "not built yet" from
 * "the fix does not work" — on 2026-09-13 that ambiguity cost several rounds of
 * reinstalling a plugin that was already correct.
 *
 * Pure: mtimes are passed in. Production wiring is [CajetaSourceMountGlue].
 */
object ArchiveHealth {

    enum class State {
        /** Present, and no older than the sources it was built from. */
        OK,

        /** No archive: the project has never been built, or not for this flavor. */
        ABSENT,

        /** Present but older than a source — still used, and worth saying. */
        STALE,
    }

    /**
     * Judged by MTIME, never by version string. A `0.1.3` archive resolved a
     * `0.2.0` tree correctly (measured 2026-09-13) because the declared types had
     * not moved, so a version mismatch is not by itself a defect — and this
     * function is given no version to be tempted by.
     *
     * An unknown source mtime yields [State.OK] rather than a guess: warning on
     * something unknowable would cry wolf, and the archive still resolves.
     */
    fun assess(archiveMtime: Long?, newestSourceMtime: Long?, declared: Boolean = true): State = when {
        // A project that declares no artifact has nothing to report. Saying
        // "not built" about every application project would be a permanent false
        // notice, and a notice that is always wrong is one nobody reads.
        !declared -> State.OK
        archiveMtime == null -> State.ABSENT
        newestSourceMtime != null && archiveMtime < newestSourceMtime -> State.STALE
        else -> State.OK
    }

    /**
     * The sentence shown for a degraded state, or null when healthy.
     *
     * Both name a remedy, and both are phrased as facts about the PROJECT rather
     * than about the edited file — the source is fine; the build is what is
     * missing or behind (spec §4.3).
     */
    fun reason(state: State): String? = when (state) {
        State.OK -> null
        State.ABSENT ->
            "project not built, so its own types cannot resolve here — run `cajeta build`"
        State.STALE ->
            "the project's archive is older than its sources, so edits to them are not " +
                "visible here yet — rebuild to pick them up"
    }
}
