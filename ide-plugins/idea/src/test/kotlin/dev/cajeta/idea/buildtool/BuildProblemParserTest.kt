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
 * json-diagnostics U4: `BuildProblemParser` turns the compiler's NDJSON into
 * build-tree problems (navigable when the record carries a file). JSON-only —
 * a non-NDJSON line is not a diagnostic.
 */
class BuildProblemParserTest {

    @Test
    fun jsonErrorWithLocationIsNavigableError() {
        val p = BuildProblemParser()
        val prob = p.feed(
            """{"severity":"error","code":"CJ1","message":"bad type","file":"/p/A.cajeta","line":10,"column":5}""")!!
        assertEquals(Diagnostic.Severity.ERROR, prob.severity)
        assertEquals("/p/A.cajeta", prob.file)
        assertEquals(10, prob.line)
        assertTrue(prob.message.contains("bad type") && prob.message.contains("CJ1"))
        val ev = prob.toParsed("b").event
        assertTrue(ev is FileMessageEventImpl)
        assertEquals(9, (ev as FileMessageEventImpl).filePosition.startLine) // 1-based -> 0-based
        assertEquals(4, ev.filePosition.startColumn)
        assertTrue(p.sawError)
    }

    @Test
    fun jsonWarningIsPositionlessWhenNoFile() {
        val p = BuildProblemParser()
        val prob = p.feed("""{"severity":"warning","message":"unused","file":null,"line":null,"column":null}""")!!
        assertEquals(Diagnostic.Severity.WARNING, prob.severity)
        assertNull(prob.file)
        assertFalse(prob.isError)
        val ev = prob.toParsed("b").event
        assertTrue(ev is MessageEventImpl && ev !is FileMessageEventImpl)
        assertEquals(MessageEvent.Kind.WARNING, (ev as MessageEventImpl).kind)
    }

    @Test
    fun noteMapsToWeakWarning() {
        val prob = BuildProblemParser().feed("""{"severity":"note","message":"fyi"}""")!!
        assertEquals(Diagnostic.Severity.WEAK_WARNING, prob.severity)
    }

    @Test
    fun nonJsonLineIsIgnoredAndDoesNotFlagError() {
        val p = BuildProblemParser()
        assertNull(p.feed("cajeta build: compiler exited 1"))
        assertNull(p.feed("Compiling module com.example.util…"))
        assertFalse(p.sawError)
    }
}
