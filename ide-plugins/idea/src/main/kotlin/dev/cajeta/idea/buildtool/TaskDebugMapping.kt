package dev.cajeta.idea.buildtool

import java.io.File

/** Debug-launch parameters for a runnable task: the artifact to launch under
 *  `cajeta dap` and the working directory (the manifest's dir). */
data class TaskDebugLaunch(
    val task: String,
    val artifactPath: String,
    val workDir: String,
)

/**
 * Maps a discovered task to debug-launch parameters (spec §5.2.2). A task is
 * debuggable only when it produces a runnable artifact whose path the discovery
 * contract reports (`runnable` + `artifact`, added to `cajeta tasks --json`);
 * the artifact is resolved relative to the manifest's directory. Returns null
 * when the task isn't runnable or its artifact path is unknown — the UI disables
 * Debug in that case rather than guessing a binary. Pure; no `com.intellij.*`.
 */
object TaskDebugMapping {

    fun launchFor(task: CajetaTask, manifestPath: String): TaskDebugLaunch? {
        if (!task.runnable) return null
        val artifact = task.artifact?.takeIf { it.isNotBlank() } ?: return null
        val dir = File(manifestPath).parentFile?.path ?: "."
        val resolved = if (File(artifact).isAbsolute) artifact else File(dir, artifact).path
        return TaskDebugLaunch(task = task.name, artifactPath = resolved, workDir = dir)
    }

    fun isDebuggable(task: CajetaTask): Boolean =
        task.runnable && !task.artifact.isNullOrBlank()
}
