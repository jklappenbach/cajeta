package dev.cajeta.idea.buildtool

/**
 * The task-discovery model — the plugin-side mirror of the `cajeta tasks --json`
 * contract (spec §3). Plain data, no `com.intellij.*`, so the tool window, run
 * configs, and tests all share one representation.
 */
data class TaskParam(
    val name: String,
    val type: String = "string",
    val default: String? = null,
    val required: Boolean = false,
    val doc: String? = null,
)

data class CajetaTask(
    val name: String,
    val description: String? = null,
    val dependsOn: List<String> = emptyList(),
    val params: List<TaskParam> = emptyList(),
    // §5.2.2: whether the task yields a runnable native artifact (so the IDE can
    // offer Debug), and where the build action writes it (when declared).
    val runnable: Boolean = false,
    val artifact: String? = null,
)

data class BuiltinCommand(
    val name: String,
    val description: String? = null,
)

/**
 * Project-level debug-launch coordinates from the document's `build` object
 * (spec §5.2.2). `cajeta dap` JIT-runs an [entryMethod] from a [sourceRoot] — it
 * does not load a prebuilt artifact — so a runnable task's Debug launch is
 * formed from these, not from [CajetaTask.artifact]. Present only when the
 * manifest's `settings.build` declares an entry method.
 */
data class BuildLaunchCoords(
    val sourceRoot: String? = null,
    val entryMethod: String,
)

data class TaskModel(
    val manifest: String,
    val tasks: List<CajetaTask>,
    val builtins: List<BuiltinCommand>,
    val buildCoords: BuildLaunchCoords? = null,
)
