package dev.cajeta.idea.settings

import java.io.File

/**
 * Is a configured path usable as an executable? Pure (string + filesystem
 * stat), no platform, so it unit-tests directly.
 *
 * Shared by the compiler path and the build-tool path. It was the build tool's
 * alone, which is how the compiler path — the one everything routes through:
 * lint, xref, Ctrl-click, run configurations, the debugger — ended up with no
 * feedback at all while the build tool had a red label and a version note.
 * [label] names the field so one vocabulary serves both.
 */
object ExecutablePathValidator {

    sealed interface Result {
        /** Usable as configured. [note] explains why it could not be fully
         *  verified here (e.g. a PATH lookup deferred to spawn time). */
        data class Ok(val note: String? = null) : Result
        data class Invalid(val reason: String) : Result
    }

    fun validate(path: String, label: String): Result {
        val trimmed = path.trim()
        if (trimmed.isEmpty()) return Result.Invalid("$label is empty")

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
    fun problem(path: String, label: String): String? =
        (validate(path, label) as? Result.Invalid)?.reason
}
