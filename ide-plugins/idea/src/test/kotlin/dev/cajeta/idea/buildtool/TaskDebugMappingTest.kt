package dev.cajeta.idea.buildtool

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * W-buildtool unit 7.1.1: mapping a runnable task to debug-launch params. Now
 * possible because `cajeta tasks --json` exposes `runnable`/`artifact`.
 */
class TaskDebugMappingTest {

    @Test
    fun mapsRunnableTaskToResolvedArtifactLaunch() {
        val task = CajetaTask("build", runnable = true, artifact = "build/exe/demo")
        val launch = TaskDebugMapping.launchFor(task, "/proj/cajeta.json")
        assertEquals("build", launch!!.task)
        assertEquals(File("/proj", "build/exe/demo").path, launch.artifactPath)  // relative -> manifest dir
        assertEquals("/proj", launch.workDir)
        assertTrue(TaskDebugMapping.isDebuggable(task))
    }

    @Test
    fun absoluteArtifactIsKeptAsIs() {
        val task = CajetaTask("build", runnable = true, artifact = "/opt/out/demo")
        assertEquals("/opt/out/demo", TaskDebugMapping.launchFor(task, "/proj/cajeta.json")!!.artifactPath)
    }

    @Test
    fun nonRunnableOrArtifactlessTaskIsNotDebuggable() {
        assertNull(TaskDebugMapping.launchFor(CajetaTask("lint", runnable = false), "/p/cajeta.json"))
        // runnable but no artifact path reported -> can't locate a binary, so null.
        assertNull(TaskDebugMapping.launchFor(CajetaTask("run", runnable = true, artifact = null), "/p/cajeta.json"))
        assertFalse(TaskDebugMapping.isDebuggable(CajetaTask("run", runnable = true, artifact = "")))
    }
}
