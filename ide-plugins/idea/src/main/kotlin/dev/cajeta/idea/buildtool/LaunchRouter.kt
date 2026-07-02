package dev.cajeta.idea.buildtool

/**
 * The single dispatch site (spec §2.6): classify a launch with [BuildRouting] and
 * invoke the matching terminal action. Pure — the two actions are passed as
 * thunks — so every entry point (double-click, Run Task, Run-with-args, saved
 * config) routes identically and the decision is unit-tested off-platform.
 */
object LaunchRouter {

    fun route(
        node: TaskTreeNode,
        model: TaskModel,
        buildWindowEnabled: Boolean,
        toBuildWindow: () -> Unit,
        toRunWindow: () -> Unit,
    ) {
        if (BuildRouting.isBuildRouted(node, model, buildWindowEnabled)) toBuildWindow() else toRunWindow()
    }
}
