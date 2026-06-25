package dev.cajeta.idea.buildtool

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * W-buildtool unit 5.1.1: the plain-JVM task-tree model builder. Groups into
 * Tasks / Built-in, sorts each, surfaces the description as tooltip, and drops
 * empty groups.
 */
class TaskTreeModelBuilderTest {

    private fun model(
        tasks: List<CajetaTask> = emptyList(),
        builtins: List<BuiltinCommand> = emptyList(),
    ) = TaskModel("/m/cajeta.json", tasks, builtins)

    @Test
    fun groupsTasksAndBuiltinsSortedWithTooltips() {
        val m = model(
            tasks = listOf(
                CajetaTask("zip", description = "Package it"),
                CajetaTask("build", description = "Compile + link"),
            ),
            builtins = listOf(
                BuiltinCommand("tasks", "List tasks"),
                BuiltinCommand("add", "Add a dependency"),
            ),
        )
        val groups = TaskTreeModelBuilder.build(m)
        assertEquals(listOf("Tasks", "Built-in"), groups.map { it.title })

        val tasks = groups[0]
        assertEquals(listOf("build", "zip"), tasks.nodes.map { it.label })   // sorted
        assertEquals("Compile + link", tasks.nodes[0].tooltip)
        assertEquals(TaskTreeNode.Kind.TASK, tasks.nodes[0].kind)
        assertEquals("build", tasks.nodes[0].runName)

        val builtins = groups[1]
        assertEquals(listOf("add", "tasks"), builtins.nodes.map { it.label })  // sorted
        assertEquals(TaskTreeNode.Kind.BUILTIN, builtins.nodes[0].kind)
    }

    @Test
    fun emptyGroupsAreDropped() {
        val onlyTasks = TaskTreeModelBuilder.build(model(tasks = listOf(CajetaTask("build"))))
        assertEquals(listOf("Tasks"), onlyTasks.map { it.title })

        val onlyBuiltins = TaskTreeModelBuilder.build(model(builtins = listOf(BuiltinCommand("info"))))
        assertEquals(listOf("Built-in"), onlyBuiltins.map { it.title })

        assertTrue(TaskTreeModelBuilder.build(model()).isEmpty())
    }

    @Test
    fun blankDescriptionBecomesNullTooltip() {
        val groups = TaskTreeModelBuilder.build(model(tasks = listOf(CajetaTask("build", description = "   "))))
        assertNull(groups[0].nodes[0].tooltip)
    }
}
