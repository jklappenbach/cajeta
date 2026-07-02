package dev.cajeta.idea.buildtool

/**
 * Decides whether a task launch belongs in the IDE's native Build tool window or
 * the Run window (spec §2). Build-family launches — every builtin pipeline verb
 * except `run`, and every user task — go to the Build window; only the builtin
 * `run` verb (which executes the user's program) uses the Run window. Debugging
 * is a separate explicit action (Debug window), unaffected by this.
 *
 * A task's `runnable`/`isDebuggable` flag means it *produces* a runnable artifact
 * (so Debug can be offered) — NOT that running it executes the program. A `build`
 * task produces an executable but doesn't run it, so it is build-routed. The
 * single classification site (spec §2.6): every launch entry point consults this.
 * Pure — no `com.intellij.*` — so callers pass the setting as a plain flag (§6).
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
            TaskTreeNode.Kind.TASK -> true
        }
    }
}
