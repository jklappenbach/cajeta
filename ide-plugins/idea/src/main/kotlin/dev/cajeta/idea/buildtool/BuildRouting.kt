package dev.cajeta.idea.buildtool

/**
 * Decides whether a task launch belongs in the IDE's native Build tool window or
 * the Run window (spec §2). Build-family launches — artifact-producing builtin
 * verbs and non-debuggable user tasks — go to the Build window; the executing
 * `run` verb and debuggable user tasks (which run the user's program) stay on the
 * Run window. The single classification site (spec §2.6): every launch entry
 * point consults this. Pure — no `com.intellij.*` — so callers pass the setting
 * as a plain flag (spec §6).
 */
object BuildRouting {

    /** Builtin verbs that execute the user's program; their stdout is program
     *  output, so they belong on the Run window. Every other builtin is a
     *  non-executing pipeline action (validate/compile/test/package/install/
     *  deploy/…) and routes to Build. */
    val RUN_ROUTED_BUILTINS = setOf("run")

    fun isBuildRouted(node: TaskTreeNode, model: TaskModel, buildWindowEnabled: Boolean): Boolean {
        if (!buildWindowEnabled) return false
        return when (node.kind) {
            TaskTreeNode.Kind.BUILTIN -> node.runName !in RUN_ROUTED_BUILTINS
            TaskTreeNode.Kind.TASK -> {
                val task = model.tasks.firstOrNull { it.name == node.runName }
                task == null || !TaskDebugMapping.isDebuggable(task, model)
            }
        }
    }
}
