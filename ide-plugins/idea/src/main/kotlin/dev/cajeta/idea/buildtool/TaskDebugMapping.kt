package dev.cajeta.idea.buildtool

import java.io.File

/**
 * Debug-launch parameters for a runnable task. `cajeta dap` JIT-runs an
 * [entryMethod] from a [sourceRoot] (it does **not** load a prebuilt binary), so
 * these mirror the Phase-2 debug launch ([dev.cajeta.idea.debugger.CajetaDebugSession.LaunchParams]):
 * the project entry method, the source root to compile (resolved against the
 * manifest's directory), and the working directory.
 */
data class TaskDebugLaunch(
    val task: String,
    val entryMethod: String,
    val sourceRoot: String,
    val workDir: String,
)

/**
 * Maps a discovered task to debug-launch parameters (spec §5.2.2). A task is
 * debuggable only when it produces a runnable artifact **and** the project
 * exposes debug-launch coordinates (`build.entryMethod`, added to
 * `cajeta tasks --json`) — because the dap server JIT-runs an entry method from
 * a source root, not the build's output binary. The source root is resolved
 * relative to the manifest's directory. Returns null when the task isn't
 * runnable or no entry method is known — the UI disables Debug in that case
 * rather than launching something that can't attach. Pure; no `com.intellij.*`.
 */
object TaskDebugMapping {

    fun launchFor(task: CajetaTask, model: TaskModel, manifestPath: String): TaskDebugLaunch? {
        if (!task.runnable) return null
        val coords = model.buildCoords ?: return null
        val dir = File(manifestPath).parentFile?.path ?: "."
        val src = coords.sourceRoot?.takeIf { it.isNotBlank() }
        val resolvedSrc = when {
            src == null -> dir
            File(src).isAbsolute -> src
            else -> File(dir, src).path
        }
        return TaskDebugLaunch(
            task = task.name,
            entryMethod = coords.entryMethod,
            sourceRoot = resolvedSrc,
            workDir = dir,
        )
    }

    fun isDebuggable(task: CajetaTask, model: TaskModel): Boolean =
        task.runnable && model.buildCoords != null
}
