package dev.cajeta.idea.buildtool

/**
 * A node in the build-tool task tree: a runnable task or built-in subcommand,
 * with the description surfaced as tooltip/secondary text (spec §4.2).
 */
data class TaskTreeNode(
    val label: String,
    val tooltip: String?,
    val kind: Kind,
) {
    enum class Kind { TASK, BUILTIN }

    /** The name passed to `cajeta` to run this node. */
    val runName: String get() = label
}

/** A titled group of nodes ("Tasks" / "Built-in"). */
data class TaskTreeGroup(val title: String, val nodes: List<TaskTreeNode>)

/**
 * Builds the grouped, sorted tree model from a [TaskModel] (spec §4). Pure — no
 * Swing, no `com.intellij.*` — so the grouping/sorting/tooltip logic is
 * unit-tested independent of the tree UI. Empty groups are dropped so the tree
 * never shows an empty "Tasks"/"Built-in" header.
 */
object TaskTreeModelBuilder {

    const val GROUP_TASKS = "Tasks"
    const val GROUP_BUILTINS = "Built-in"

    fun build(model: TaskModel): List<TaskTreeGroup> {
        val groups = mutableListOf<TaskTreeGroup>()
        val taskNodes = model.tasks
            .sortedBy { it.name }
            .map { TaskTreeNode(it.name, it.description?.takeIf(String::isNotBlank), TaskTreeNode.Kind.TASK) }
        if (taskNodes.isNotEmpty()) groups += TaskTreeGroup(GROUP_TASKS, taskNodes)

        val builtinNodes = model.builtins
            .sortedBy { it.name }
            .map { TaskTreeNode(it.name, it.description?.takeIf(String::isNotBlank), TaskTreeNode.Kind.BUILTIN) }
        if (builtinNodes.isNotEmpty()) groups += TaskTreeGroup(GROUP_BUILTINS, builtinNodes)

        return groups
    }
}
