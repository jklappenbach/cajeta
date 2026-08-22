package dev.cajeta.idea.buildtool

/**
 * Which console stream a line should RENDER as, which is not always the stream
 * it arrived on.
 *
 * IntelliJ's Build window colours by stream: stdout renders in the default
 * colour, stderr in red. `OutputBuildEventImpl` carries a single `stdout`
 * boolean, so those are the only two states available — there is no third
 * "informational" colour to reach for.
 *
 * That matters because cajeta's build tool writes plugin PROGRESS to stderr.
 * `PluginRuntime.cpp` routes every `kind: "log"` record there, so an ordinary
 * cajeta-coco run —
 *
 *     [plugin] coco: [1/6] reference pass
 *     [plugin] coco: [2/6] IR + xref pass
 *     [plugin] coco: [3/6] instrumenting 6 of 10 modules
 *
 * — arrives as six consecutive red lines and reads as six errors. The run is
 * fine. Julian saw exactly this and asked why everything was red.
 *
 * Fixing the compiler to write `log` records to stdout is the right long-term
 * change, but it only helps people on a toolchain new enough to have it. This
 * classifier fixes the rendering for every toolchain, including the one already
 * installed.
 *
 * The rule is deliberately narrow: a line is re-labelled as informational only
 * when it carries a known progress marker. Anything unrecognised stays on
 * stderr and stays red — the failure mode to avoid is painting a real error
 * white, which is far worse than a warning that stayed red.
 */
object ConsoleStreamClassifier {

    /**
     * Markers that identify a line as progress rather than a problem.
     *
     * `[plugin] ` is the build tool's prefix for a plugin's `kind: "log"`
     * record, which is informational by definition — a plugin reports failures
     * through its `result` record, which surfaces as a task error, not as this.
     */
    private val INFORMATIONAL_PREFIXES = listOf(
        "[plugin] ",
        "[incremental] ",
        "[cache] ",
    )

    /**
     * Substrings that veto the re-label even on an otherwise informational
     * line. A plugin is free to log the word "error"; if it does, leave the
     * line red rather than deciding we know better.
     */
    private val PROBLEM_MARKERS = listOf(
        "error:", "error :", "ERROR", "failed:", "FAILED",
        "warning:", "uncaught exception", "SIGSEGV", "SIGABRT",
    )

    /**
     * @param stdout the stream the line actually arrived on.
     * @return the stream it should RENDER as.
     */
    fun renderAsStdout(line: String, stdout: Boolean): Boolean {
        if (stdout) return true
        val trimmed = line.trimStart()
        if (INFORMATIONAL_PREFIXES.none { trimmed.startsWith(it) }) return false
        if (PROBLEM_MARKERS.any { line.contains(it) }) return false
        return true
    }
}
