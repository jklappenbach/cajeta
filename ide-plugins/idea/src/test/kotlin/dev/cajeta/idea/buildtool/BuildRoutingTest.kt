package dev.cajeta.idea.buildtool

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * idea-build-toolwindow U1: the pure build-vs-run classifier (spec §2). Artifact-
 * producing builtin verbs and non-debuggable user tasks route to the Build tool
 * window; the executing `run` verb and debuggable tasks stay on the Run window.
 * Pure — no `com.intellij.*` — so routing is unit-tested off-platform.
 */
class BuildRoutingTest {

    private fun builtin(name: String) = TaskTreeNode(name, null, TaskTreeNode.Kind.BUILTIN)
    private fun task(name: String) = TaskTreeNode(name, null, TaskTreeNode.Kind.TASK)

    private fun model(vararg tasks: CajetaTask, coords: BuildLaunchCoords? = null) =
        TaskModel(manifest = "/proj/cajeta.json", tasks = tasks.toList(), builtins = emptyList(), buildCoords = coords)

    @Test
    fun artifactBuiltinsRouteToBuildWindow() {
        val m = model()
        for (verb in listOf("validate", "compile", "test", "package", "install", "deploy")) {
            assertTrue("`$verb` should be build-routed", BuildRouting.isBuildRouted(builtin(verb), m, true))
        }
    }

    @Test
    fun runBuiltinStaysOnRunWindow() {
        assertFalse(BuildRouting.isBuildRouted(builtin("run"), model(), true))
    }

    @Test
    fun nonDebuggableUserTaskRoutesToBuild_debuggableStaysOnRun() {
        // Debuggable: runnable + project has debug coords -> Run window.
        val runnable = CajetaTask("bench", runnable = true)
        val debuggable = model(runnable, coords = BuildLaunchCoords(entryMethod = "a.B::main"))
        assertFalse(BuildRouting.isBuildRouted(task("bench"), debuggable, true))

        // Not debuggable (no coords) -> Build window.
        val noCoords = model(runnable, coords = null)
        assertTrue(BuildRouting.isBuildRouted(task("bench"), noCoords, true))

        // Not runnable -> Build window.
        val notRunnable = model(CajetaTask("codegen", runnable = false), coords = BuildLaunchCoords(entryMethod = "a.B::main"))
        assertTrue(BuildRouting.isBuildRouted(task("codegen"), notRunnable, true))

        // Unknown task (not in model) -> Build window (non-executing default).
        assertTrue(BuildRouting.isBuildRouted(task("mystery"), model(), true))
    }

    @Test
    fun settingOffForcesEveryLaunchToRunWindow() {
        val m = model(CajetaTask("codegen", runnable = false))
        assertFalse(BuildRouting.isBuildRouted(builtin("compile"), m, false))
        assertFalse(BuildRouting.isBuildRouted(task("codegen"), m, false))
    }
}
