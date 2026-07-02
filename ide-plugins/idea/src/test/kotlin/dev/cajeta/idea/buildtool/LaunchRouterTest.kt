package dev.cajeta.idea.buildtool

import com.intellij.build.BuildProgressListener
import com.intellij.build.events.BuildEvent
import com.intellij.build.events.FinishBuildEvent
import com.intellij.build.events.impl.FailureResultImpl
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * idea-build-toolwindow U4: every launch entry point routes through one
 * classifier (spec §2.6) — build-family to the Build window action, run/debuggable
 * to the Run window action; and a parsed compiler error flips the streamed build
 * to failure (spec §4.5). Both verified off-platform with thunks / fakes.
 */
class LaunchRouterTest {

    private fun builtin(n: String) = TaskTreeNode(n, null, TaskTreeNode.Kind.BUILTIN)
    private fun task(n: String) = TaskTreeNode(n, null, TaskTreeNode.Kind.TASK)
    private fun model(vararg t: CajetaTask, coords: BuildLaunchCoords? = null) =
        TaskModel("/p/cajeta.json", t.toList(), emptyList(), coords)

    private fun routed(node: TaskTreeNode, model: TaskModel, enabled: Boolean = true): String {
        var hit = "none"
        LaunchRouter.route(node, model, enabled, { hit = "build" }, { hit = "run" })
        return hit
    }

    @Test
    fun buildFamilyGoesToBuildWindow_runAndDebuggableGoToRun() {
        assertEquals("build", routed(builtin("compile"), model()))
        assertEquals("build", routed(builtin("package"), model()))
        assertEquals("run", routed(builtin("run"), model()))
        // user task, not debuggable -> build; debuggable -> run
        assertEquals("build", routed(task("gen"), model(CajetaTask("gen", runnable = false))))
        assertEquals("run", routed(task("bench"),
            model(CajetaTask("bench", runnable = true), coords = BuildLaunchCoords(entryMethod = "a.B::main"))))
    }

    @Test
    fun settingOffSendsEverythingToRun() {
        assertEquals("run", routed(builtin("compile"), model(), enabled = false))
        assertEquals("run", routed(task("gen"), model(CajetaTask("gen")), enabled = false))
    }

    @Test
    fun parsedCompilerErrorFlipsStreamedBuildToFailure() {
        // Wire the real parser into the bridge (as production does) and stream an
        // ANTLR error line: exit 0 but a parsed ERROR must finish the build failed.
        val recorded = mutableListOf<BuildEvent>()
        val listener = BuildProgressListener { _, e -> recorded.add(e) }
        val parser = BuildProblemParser()
        val lineParser = LineParser { line, pid -> parser.feed(line)?.toParsed(pid) }
        val process = object : BuildTaskProcess {
            override fun run(sink: OutputSink): ProcessOutcome {
                sink.append("line 4:46 missing ';' at '}'\n", stdout = false)
                return ProcessOutcome(exitCode = 0, cancelled = false)
            }
            override fun cancel() {}
        }
        val result = CajetaBuildBridge.execute(listener, "b", "cajeta compile", "/p", 0L, { null }, process, lineParser)
        assertEquals(CajetaBuildBridge.Result.FAILURE, result)
        assertTrue((recorded.last() as FinishBuildEvent).result is FailureResultImpl)
    }
}
