package dev.cajeta.idea.buildtool

import com.intellij.build.BuildProgressListener
import com.intellij.build.events.BuildEvent
import com.intellij.build.events.FinishBuildEvent
import com.intellij.build.events.OutputBuildEvent
import com.intellij.build.events.StartBuildEvent
import com.intellij.build.events.impl.FailureResultImpl
import com.intellij.build.events.impl.SkippedResultImpl
import com.intellij.build.events.impl.SuccessResultImpl
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * idea-build-toolwindow U2: the build-event bridge drives a `BuildProgressListener`
 * (the native Build tool window's `BuildViewManager` in production) through
 * Start → Output* → Finish for a task run (spec §3, §5.1). Tested off-platform
 * with a recording listener and a scripted fake process — no IDE required.
 */
class CajetaBuildBridgeTest {

    private class Recording : BuildProgressListener {
        val events = mutableListOf<Pair<Any, BuildEvent>>()
        override fun onEvent(buildId: Any, event: BuildEvent) { events.add(buildId to event) }
    }

    private class FakeProcess(val script: (OutputSink) -> ProcessOutcome) : BuildTaskProcess {
        var ran = false
        override fun run(sink: OutputSink): ProcessOutcome { ran = true; return script(sink) }
        override fun cancel() {}
    }

    private val BID = "build-1"

    private fun run(process: BuildTaskProcess, preflight: () -> String? = { null }): Pair<Recording, CajetaBuildBridge.Result> {
        val l = Recording()
        val r = CajetaBuildBridge.execute(l, BID, "cajeta compile", "/proj", 0L, preflight, process)
        return l to r
    }

    @Test
    fun successEmitsStartThenOutputThenFinishSuccess_sameBuildId() {
        val (l, result) = run(FakeProcess { sink ->
            sink.append("compiling…\n", stdout = true)
            ProcessOutcome(exitCode = 0, cancelled = false)
        })
        assertEquals(CajetaBuildBridge.Result.SUCCESS, result)
        assertTrue("first event is Start", l.events.first().second is StartBuildEvent)
        assertTrue("last event is Finish", l.events.last().second is FinishBuildEvent)
        assertTrue((l.events.last().second as FinishBuildEvent).result is SuccessResultImpl)
        assertTrue("an output event exists", l.events.any { it.second is OutputBuildEvent })
        assertTrue("all events share the buildId", l.events.all { it.first == BID })
    }

    @Test
    fun nonZeroExitFinishesFailure() {
        val (l, result) = run(FakeProcess { ProcessOutcome(exitCode = 2, cancelled = false) })
        assertEquals(CajetaBuildBridge.Result.FAILURE, result)
        assertTrue((l.events.last().second as FinishBuildEvent).result is FailureResultImpl)
    }

    @Test
    fun cancelledRunFinishesSkipped() {
        val (l, result) = run(FakeProcess { ProcessOutcome(exitCode = -1, cancelled = true) })
        assertEquals(CajetaBuildBridge.Result.CANCELLED, result)
        assertTrue((l.events.last().second as FinishBuildEvent).result is SkippedResultImpl)
    }

    @Test
    fun stdoutAndStderrBothSurfaceWithCorrectFlags() {
        val (l, _) = run(FakeProcess { sink ->
            sink.append("to out\n", stdout = true)
            sink.append("to err\n", stdout = false)
            ProcessOutcome(exitCode = 0, cancelled = false)
        })
        val outputs = l.events.map { it.second }.filterIsInstance<OutputBuildEvent>()
        assertTrue(outputs.any { it.isStdOut && it.message.contains("to out") })
        assertTrue(outputs.any { !it.isStdOut && it.message.contains("to err") })
    }

    @Test
    fun concurrentBuildsKeepDistinctBuildIdsAndOutput() {
        val l = Recording()
        CajetaBuildBridge.execute(l, "b-A", "cajeta compile", "/p", 0L, { null },
            FakeProcess { s -> s.append("A-out\n", true); ProcessOutcome(0, false) })
        CajetaBuildBridge.execute(l, "b-B", "cajeta package", "/p", 0L, { null },
            FakeProcess { s -> s.append("B-out\n", true); ProcessOutcome(0, false) })
        val aOut = l.events.filter { it.first == "b-A" }.map { it.second }.filterIsInstance<OutputBuildEvent>()
        val bOut = l.events.filter { it.first == "b-B" }.map { it.second }.filterIsInstance<OutputBuildEvent>()
        assertTrue(aOut.all { it.message.contains("A-out") } && aOut.isNotEmpty())
        assertTrue(bOut.all { it.message.contains("B-out") } && bOut.isNotEmpty())
    }

    @Test
    fun preflightProblemFinishesFailureWithoutSpawning() {
        val proc = FakeProcess { ProcessOutcome(0, false) }
        val (l, result) = run(proc, preflight = { "build tool path: not executable" })
        assertEquals(CajetaBuildBridge.Result.FAILURE, result)
        assertFalse("process must not be spawned when preflight fails", proc.ran)
        val fin = l.events.last().second as FinishBuildEvent
        assertTrue(fin.result is FailureResultImpl)
    }
}
