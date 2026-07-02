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
    fun everyUserTaskRoutesToBuild_evenRunnableArtifactProducers() {
        // A `build` task that PRODUCES a runnable executable (runnable=true) in a
        // project that has an entry method is still a build action, not a program
        // run -> Build window. (Regression: this used to mis-route to Run because
        // isDebuggable conflated "produces a runnable artifact" with "runs it".)
        val buildTask = CajetaTask("build", runnable = true, artifact = "build/api")
        val withCoords = model(buildTask, coords = BuildLaunchCoords(entryMethod = "com.example.api.Main::main"))
        assertTrue(BuildRouting.isBuildRouted(task("build"), withCoords, true))

        // Non-runnable task and unknown task also -> Build.
        assertTrue(BuildRouting.isBuildRouted(task("codegen"), model(CajetaTask("codegen", runnable = false)), true))
        assertTrue(BuildRouting.isBuildRouted(task("mystery"), model(), true))
    }

    @Test
    fun settingOffForcesEveryLaunchToRunWindow() {
        val m = model(CajetaTask("codegen", runnable = false))
        assertFalse(BuildRouting.isBuildRouted(builtin("compile"), m, false))
        assertFalse(BuildRouting.isBuildRouted(task("codegen"), m, false))
    }
}
