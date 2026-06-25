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
}
