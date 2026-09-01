package dev.cajeta.idea.profiler

/**
 * What the profiler window says when it is holding no trace (spec §5.4).
 *
 * Kept out of the Swing component on purpose: the decision and the wording are
 * the parts worth testing, and a panel needs an IDE fixture to instantiate.
 * Until this existed the window showed bare tabs — no trace, no explanation,
 * and no way to get one; the only route was a Tools-menu action nobody looks
 * for, which is where `cajeta-profiler` 11.3.a stalled.
 */
object ProfilerEmptyState {

    const val TITLE = "No profile loaded"

    /** True while the window holds no model. An EMPTY trace is still loaded —
     *  the window then reports what it contains, which is a different claim. */
    fun shouldShow(model: ProfileViewModel?): Boolean = model == null

    /**
     * Both routes, because the two developers who arrive here arrive by
     * different paths: one profiled from a shell and is looking for the file,
     * the other expects a run to have opened it.
     */
    fun message(): String =
        "Open a .pftrace written by a profiled run, or start a run with " +
        "profiling armed and this window opens by itself. From a shell, set " +
        "CAJETA_PROFILER=1 on the run."
}
