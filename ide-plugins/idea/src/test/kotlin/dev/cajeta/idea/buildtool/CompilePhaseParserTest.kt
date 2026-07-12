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

    /** Start and finish must carry the SAME node id — that is what pairs them into
     *  one node — and different builds must not share ids. */
    @Test
    fun startAndFinishShareANodeIdScopedToTheBuild() {
        val buildA = Any()
        val buildB = Any()
        val s = CompilePhaseParser.parse(start)!!.toParsed(buildA).event as StartEvent
        val f = CompilePhaseParser.parse(finish)!!.toParsed(buildA).event as FinishEvent
        val other = CompilePhaseParser.parse(start)!!.toParsed(buildB).event as StartEvent

        assertEquals("finish closes the node its start opened", s.id, f.id)
        assertEquals(buildA, s.parentId)
        assertTrue("a concurrent build gets its own node", s.id != other.id)
    }

    @Test
    fun phasesAreNotErrorsButDiagnosticsStillAre() {
        val parser = BuildOutputParser()
        val phase = parser.parse(start, Any())
        assertNotNull(phase)
        assertFalse("a phase is not a problem", phase!!.isError)
        assertFalse(parser.sawError)

        val problem = parser.parse(diagnostic, Any())!!
        assertTrue(problem.isError)
        assertTrue(problem.event is MessageEvent)
        assertTrue(parser.sawError)
    }

    /** A line the parser does not claim stays null, so the bridge echoes it as
     *  plain console output instead of swallowing it. */
    @Test
    fun plainOutputIsLeftForTheConsole() {
        val parser = BuildOutputParser()
        assertNull(parser.parse("[incremental] skip tour/Counter.cajeta", Any()))
    }
}
