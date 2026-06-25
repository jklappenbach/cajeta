package dev.cajeta.idea.buildtool

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * W-buildtool unit 7: mapping a runnable task to dap debug-launch params. The
 * dap server JIT-runs an entry method from a source root (it does NOT load a
 * prebuilt artifact), so the launch is formed from the document's `build`
 * coordinates (`entryMethod` + `sourceRoot`), now exposed by
 * `cajeta tasks --json`.
 */
class TaskDebugMappingTest {

    private fun model(task: CajetaTask, coords: BuildLaunchCoords?) =
        TaskModel(manifest = "/proj/cajeta.json", tasks = listOf(task), builtins = emptyList(), buildCoords = coords)

    @Test
    fun mapsRunnableTaskToJitLaunchWithResolvedSourceRoot() {
        val task = CajetaTask("run", runnable = true, artifact = "build/profile")
        val coords = BuildLaunchCoords(sourceRoot = "src/main/cajeta", entryMethod = "profile.Profile::main")
        val m = model(task, coords)
        val launch = TaskDebugMapping.launchFor(task, m, "/proj/cajeta.json")!!
        assertEquals("run", launch.task)
        assertEquals("profile.Profile::main", launch.entryMethod)
        assertEquals(File("/proj", "src/main/cajeta").path, launch.sourceRoot)  // relative -> manifest dir
        assertEquals("/proj", launch.workDir)
        assertTrue(TaskDebugMapping.isDebuggable(task, m))
    }

    @Test
    fun absoluteSourceRootIsKeptAndMissingSourceRootFallsToManifestDir() {
        val task = CajetaTask("run", runnable = true)
        val abs = model(task, BuildLaunchCoords(sourceRoot = "/opt/src", entryMethod = "a.B::main"))
        assertEquals("/opt/src", TaskDebugMapping.launchFor(task, abs, "/proj/cajeta.json")!!.sourceRoot)

        val none = model(task, BuildLaunchCoords(sourceRoot = null, entryMethod = "a.B::main"))
        assertEquals("/proj", TaskDebugMapping.launchFor(task, none, "/proj/cajeta.json")!!.sourceRoot)
    }

    @Test
    fun nonRunnableOrCoordlessTaskIsNotDebuggable() {
        val coords = BuildLaunchCoords(sourceRoot = "s", entryMethod = "a.B::main")
        // not runnable -> null even with coords present
        val lint = CajetaTask("lint", runnable = false)
        assertNull(TaskDebugMapping.launchFor(lint, model(lint, coords), "/p/cajeta.json"))
        // runnable but no project entry method reported -> can't JIT-run, so null.
        val run = CajetaTask("run", runnable = true)
        assertNull(TaskDebugMapping.launchFor(run, model(run, null), "/p/cajeta.json"))
        assertFalse(TaskDebugMapping.isDebuggable(run, model(run, null)))
    }
}
