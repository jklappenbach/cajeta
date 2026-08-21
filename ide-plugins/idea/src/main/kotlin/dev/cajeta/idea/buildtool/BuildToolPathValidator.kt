package dev.cajeta.idea.buildtool

import java.io.File

/**
 * Validates the configured `buildToolPath` (spec §14.2.1) so an invalid path is
 * flagged in Settings and surfaced — not silently swallowed — on first use. A
 * bare command name (no path separator) is assumed resolvable on `PATH` and
 * confirmed at spawn time; an explicit path must point at an existing,
 * executable, non-directory file. Pure (string + filesystem stat), no platform.
 */
object BuildToolPathValidator {

    sealed interface Result {
        /** Usable as configured. [note] explains why it could not be fully
         *  verified here (e.g. a PATH lookup deferred to spawn time). */
        data class Ok(val note: String? = null) : Result
        data class Invalid(val reason: String) : Result
    }

    fun validate(path: String): Result {
        val trimmed = path.trim()
        if (trimmed.isEmpty()) return Result.Invalid("Build-tool path is empty")

        // Bare command name → resolved on PATH at spawn time (can't cheaply
        // confirm here without scanning PATH; surfaced if the spawn fails).
        if (!trimmed.contains(File.separatorChar) && trimmed == File(trimmed).name) {
            return Result.Ok(note = "Resolved on PATH at run time")
        }

        val f = File(trimmed)
        return when {
            !f.exists() -> Result.Invalid("No such file: $trimmed")
            f.isDirectory -> Result.Invalid("Path is a directory, not an executable: $trimmed")
            !f.canExecute() -> Result.Invalid("Not executable: $trimmed")
            else -> Result.Ok()
        }
    }

    /** Convenience for callers that only need the message (null when valid). */
    fun problem(path: String): String? =
        (validate(path) as? Result.Invalid)?.reason

    // ── toolchain floor ─────────────────────────────────────────────────
    //
    // Separate from validate() on purpose: validate() is pure (string +
    // stat) and its tests depend on that. Asking a binary its version means
    // spawning it, so that lives here and callers opt in.

    /**
     * The oldest toolchain cajeta-coco can be driven by.
     *
     * Not a packaging preference. coco's build-tool plugin ships as a `.cja`
     * and is **AOT-compiled by whichever toolchain runs it**, which makes the
     * compiler part of coco's RUNTIME rather than just its build. 0.21.0
     * miscompiles coco's file reads — an intrinsic `#`-return losing its title
     * across a return boundary — so every coco verb throws
     *
     *     cajeta: uncaught exception (value=0x3)
     *       at cajeta.coco.plugin.Pipeline.moduleFiles(...)
     *
     * from whichever method reads a file first. Publishing a fixed plugin
     * cannot help: the bytes are already right and the compile of them is not.
     * Nothing in that stack names a version, which is why this warning exists.
     */
    const val COCO_MIN_TOOLCHAIN: String = "0.21.1"

    /**
     * `cajeta 0.21.0 (7c0f40f3)` → `0.21.0`; null when the line is not one.
     */
    fun parseVersion(versionLine: String): String? {
        val m = Regex("""\bcajeta\s+(\d+\.\d+\.\d+)""").find(versionLine) ?: return null
        return m.groupValues[1]
    }

    /** Numeric, component-wise: "0.9.4" < "0.21.1" (a string compare gets this wrong). */
    fun isBelow(version: String, floor: String): Boolean {
        fun parts(v: String) = v.split('.').map { it.toIntOrNull() ?: 0 }
        val a = parts(version)
        val b = parts(floor)
        for (i in 0 until maxOf(a.size, b.size)) {
            val x = a.getOrElse(i) { 0 }
            val y = b.getOrElse(i) { 0 }
            if (x != y) return x < y
        }
        return false
    }

    /**
     * A warning when the configured tool is below coco's floor, else null.
     *
     * Unknown output is NOT a warning: a toolchain that does not answer
     * `--version` in the expected shape is a different problem, and crying
     * wolf here would train the reader to ignore the one case that matters.
     */
    /**
     * Ask the configured tool its version and warn when it is below the floor.
     *
     * Spawns, so it is deliberately NOT part of [validate]. Any failure —
     * missing binary, non-zero exit, a hang — yields null: this is an advisory
     * about a version, and a tool that cannot be run at all is already reported
     * by [validate] with a better message than this one could give.
     */
    fun coverageFloorWarningFor(path: String, timeoutMs: Long = 3000): String? {
        val trimmed = path.trim()
        if (trimmed.isEmpty()) return null
        return try {
            val p = ProcessBuilder(trimmed, "--version")
                .redirectErrorStream(true)
                .start()
            if (!p.waitFor(timeoutMs, java.util.concurrent.TimeUnit.MILLISECONDS)) {
                p.destroyForcibly()
                return null
            }
            coverageFloorWarning(p.inputStream.bufferedReader().readText())
        } catch (e: Exception) {
            null
        }
    }

    fun coverageFloorWarning(versionLine: String): String? {
        val v = parseVersion(versionLine) ?: return null
        if (!isBelow(v, COCO_MIN_TOOLCHAIN)) return null
        return "cajeta $v is below cajeta-coco's $COCO_MIN_TOOLCHAIN floor — " +
            "coverage will fail with `uncaught exception (value=0x3)`, because " +
            "coco's plugin is compiled by this toolchain."
    }
}
