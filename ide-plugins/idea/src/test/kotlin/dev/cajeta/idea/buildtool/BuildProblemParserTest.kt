package dev.cajeta.idea.buildtool

import com.intellij.build.events.MessageEvent
import com.intellij.build.events.impl.FileMessageEventImpl
import com.intellij.build.events.impl.MessageEventImpl
import dev.cajeta.idea.lint.Diagnostic
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * idea-build-toolwindow U3: regex-parse the compiler's unstructured diagnostics
 * (three verified shapes) into build-tree problem events (spec §4). Reuses the
 * degraded-lint severity vocabulary; navigation only for the path-bearing shape.
 */
class BuildProblemParserTest {

    @Test
    fun directWarningIsPositionlessWarning() {
        val p = BuildProblemParser()
        val prob = p.feed("warning: [uncaught-throws] call to fetch can throw")!!
        assertEquals(Diagnostic.Severity.WARNING, prob.severity)
        assertNull(prob.file)
        assertTrue(prob.message.contains("call to fetch"))
        assertFalse(prob.isError)
        val parsed = prob.toParsed("b")
        assertTrue(parsed.event is MessageEventImpl && parsed.event !is FileMessageEventImpl)
        assertEquals(MessageEvent.Kind.WARNING, (parsed.event as MessageEventImpl).kind)
        assertFalse(parsed.isError)
    }

    @Test
    fun cajetaLoggerErrorPairsLocationWithMessageAndNavigates() {
        val p = BuildProblemParser()
        assertNull("location line alone is pending", p.feed("/proj/src/A.cajeta[10:5]"))
        val prob = p.feed("Error CJ123: type mismatch in return")!!
        assertEquals(Diagnostic.Severity.ERROR, prob.severity)
        assertEquals("/proj/src/A.cajeta", prob.file)
        assertEquals(10, prob.line)
        assertEquals(5, prob.column)
        assertTrue(prob.message.contains("type mismatch"))
        val ev = prob.toParsed("b").event
        assertTrue(ev is FileMessageEventImpl)
        assertEquals(9, (ev as FileMessageEventImpl).filePosition.startLine) // 1-based -> 0-based
        assertEquals(5, ev.filePosition.startColumn)
        assertTrue(p.sawError)
    }

    @Test
    fun glogPrefixedLocationAndCommaVariantParse() {
        val p = BuildProblemParser()
        assertNull(p.feed("E0702 15:32:01.123456 12345 CajetaLogger.cpp:51] /proj/src/B.cajeta[7,3]"))
        val prob = p.feed("Error CJ9: bad thing")!!
        assertEquals("/proj/src/B.cajeta", prob.file)
        assertEquals(7, prob.line)
        assertEquals(3, prob.column)
    }

    @Test
    fun antlrSyntaxErrorHasLineColButNoFile() {
        val p = BuildProblemParser()
        val prob = p.feed("line 4:46 missing ';' at '}'")!!
        assertEquals(Diagnostic.Severity.ERROR, prob.severity)
        assertNull(prob.file)
        assertEquals(4, prob.line)
        assertEquals(46, prob.column)
        assertTrue(prob.message.contains("missing ';'"))
        // No path -> positionless message node (no navigation), but not dropped.
        assertTrue(prob.toParsed("b").event.let { it is MessageEventImpl && it !is FileMessageEventImpl })
    }

    @Test
    fun nonDiagnosticLineIsIgnoredAndDoesNotFlagError() {
        val p = BuildProblemParser()
        assertNull(p.feed("Compiling module com.example.util…"))
        assertNull(p.feed(""))
        assertFalse(p.sawError)
    }

    @Test
    fun severityMapsToMessageEventKind() {
        assertEquals(MessageEvent.Kind.ERROR, BuildProblem(Diagnostic.Severity.ERROR, "e").toParsed("b").event.let { (it as MessageEventImpl).kind })
        assertEquals(MessageEvent.Kind.WARNING, BuildProblem(Diagnostic.Severity.WARNING, "w").toParsed("b").event.let { (it as MessageEventImpl).kind })
        assertEquals(MessageEvent.Kind.WARNING, BuildProblem(Diagnostic.Severity.WEAK_WARNING, "ww").toParsed("b").event.let { (it as MessageEventImpl).kind })
    }
}
