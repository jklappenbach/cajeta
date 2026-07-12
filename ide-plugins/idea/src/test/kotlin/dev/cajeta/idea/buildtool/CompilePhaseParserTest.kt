package dev.cajeta.idea.buildtool

import com.intellij.build.events.FinishEvent
import com.intellij.build.events.MessageEvent
import com.intellij.build.events.StartEvent
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Compile-phase progress records (`Diagnostics.h::emitJsonProgress`) become
 * child nodes of the build, so the Build tool window shows what the compiler is
 * doing rather than a spinner. The records ride the same NDJSON stream as the
 * diagnostics, so the two must not be confused for each other.
 */
class CompilePhaseParserTest {

    private val start =
        """{"kind":"progress","phase":"codegen","state":"start","label":"Generating code"}"""
    private val finish =
        """{"kind":"progress","phase":"codegen","state":"finish","label":"Generating code","elapsedMs":2447}"""
    private val diagnostic =
        """{"severity":"error","code":"E1","message":"boom","file":"A.cajeta","line":3,"column":1}"""

    @Test
    fun parsesStartAndFinishRecords() {
        val s = CompilePhaseParser.parse(start)!!
        assertEquals("codegen", s.phase)
        assertEquals(CompilePhase.State.START, s.state)
        assertEquals("Generating code", s.label)
        assertNull(s.elapsedMs)

        val f = CompilePhaseParser.parse(finish)!!
        assertEquals(CompilePhase.State.FINISH, f.state)
        assertEquals(2447L, f.elapsedMs)
    }

    @Test
    fun ignoresDiagnosticsAndPlainOutput() {
        assertNull("a diagnostic is not a phase", CompilePhaseParser.parse(diagnostic))
        assertNull(CompilePhaseParser.parse("[cache] hit — re-published build/tour"))
        assertNull(CompilePhaseParser.parse(""))
        assertNull(CompilePhaseParser.parse("{ not json"))
        assertNull("unknown kind is ignored", CompilePhaseParser.parse("""{"kind":"other"}"""))
    }

    /**
     * The first phase opens a "Compile" grouping node and hangs itself under it;
     * later phases hang under the same node. Start and finish of a phase share an
     * id (that is what pairs them into ONE node, so a completed phase stays in the
     * tree rather than being replaced).
     */
    @Test
    fun phasesAccumulateUnderACompileNode() {
        val build = Any()
        val parser = BuildOutputParser()

        val first = parser.parse(start, build)
        assertEquals("opens Compile, then the phase", 2, first.size)
        val compile = first[0].event as StartEvent
        val codegen = first[1].event as StartEvent
        assertEquals("Compile", compile.message)
        assertEquals("the group hangs off the build", build, compile.parentId)
        assertEquals("the phase hangs off the group", compile.id, codegen.parentId)

        // A second phase reuses the already-open group.
        val second = parser.parse(
            """{"kind":"progress","phase":"emit","state":"start","label":"Emitting objects"}""",
            build,
        )
        assertEquals("group is opened once", 1, second.size)
        assertEquals(compile.id, (second[0].event as StartEvent).parentId)

        val done = parser.parse(finish, build)
        val fin = done.single().event as FinishEvent
        assertEquals("finish closes the node its start opened", codegen.id, fin.id)
    }

    /** The whole point: a completed phase keeps the time it took. */
    @Test
    fun completedPhasePreservesItsElapsedTime() {
        val parser = BuildOutputParser()
        parser.parse(start, Any())
        val fin = parser.parse(finish, Any()).single().event as FinishEvent
        assertTrue("keeps the label", fin.message.startsWith("Generating code"))
        assertTrue("and the duration: ${fin.message}", fin.message.contains("2.4 s"))
    }

    @Test
    fun durationUnitsScaleWithMagnitude() {
        fun finishOf(ms: Long): String {
            val p = BuildOutputParser()
            val line = """{"kind":"progress","phase":"p","state":"finish","label":"L","elapsedMs":$ms}"""
            return (p.parse(line, Any()).last().event as FinishEvent).message
        }
        assertTrue(finishOf(899).contains("899 ms"))
        assertTrue(finishOf(9295).contains("9.3 s"))
        assertTrue(finishOf(64_000).contains("1 m 04 s"))
    }

    /** The grouping node must be closed when the process exits, or it renders as
     *  forever-running. Only if a phase actually opened it. */
    @Test
    fun closeShutsTheCompileNodeOnlyWhenOpened() {
        val build = Any()
        val untouched = BuildOutputParser()
        assertTrue("nothing to close when no phase ran", untouched.close(build).isEmpty())

        val parser = BuildOutputParser()
        val compileId = (parser.parse(start, build).first().event as StartEvent).id
        val closed = parser.close(build).single().event as FinishEvent
        assertEquals("closes the group it opened", compileId, closed.id)
        assertTrue("idempotent", parser.close(build).isEmpty())
    }

    /**
     * A cached build runs no compiler and so emits no phases. It must still say
     * something, or the tree is empty under an instant green check — exactly the
     * "build seems broken" report.
     */
    @Test
    fun cacheHitIsReportedInTheTree() {
        val cache = """{"kind":"cache","state":"hit","artifact":"build/tour"}"""
        val hit = CompilePhaseParser.parseCacheHit(cache)!!
        assertEquals("build/tour", hit.artifact)
        assertNull("a cache record is not a phase", CompilePhaseParser.parse(cache))

        val parser = BuildOutputParser()
        val event = parser.parse(cache, Any()).single().event as MessageEvent
        assertEquals(MessageEvent.Kind.INFO, event.kind)
        assertTrue("names the artifact: ${event.message}", event.message.contains("build/tour"))
        assertTrue("says it came from cache", event.message.contains("cache"))
        assertFalse(parser.sawError)
    }

    @Test
    fun phasesAreNotErrorsButDiagnosticsStillAre() {
        val parser = BuildOutputParser()
        val phase = parser.parse(start, Any())
        assertNotNull(phase)
        assertTrue("a phase is not a problem", phase.none { it.isError })
        assertFalse(parser.sawError)

        val problem = parser.parse(diagnostic, Any()).single()
        assertTrue(problem.isError)
        assertTrue(problem.event is MessageEvent)
        assertTrue(parser.sawError)
    }

    /** A line the parser does not claim yields nothing, so the bridge echoes it as
     *  plain console output instead of swallowing it. */
    @Test
    fun plainOutputIsLeftForTheConsole() {
        val parser = BuildOutputParser()
        assertTrue(parser.parse("[incremental] skip tour/Counter.cajeta", Any()).isEmpty())
    }
}
