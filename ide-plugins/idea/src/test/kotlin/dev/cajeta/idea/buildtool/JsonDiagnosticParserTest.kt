package dev.cajeta.idea.buildtool

import dev.cajeta.idea.lint.Diagnostic
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * json-diagnostics U2: parse one NDJSON diagnostic line
 * (`{severity,code,message,file,line,column}`) into a structured record, reusing
 * the bundled `Json` DOM and the degraded-lint severity vocabulary. Pure /
 * off-platform.
 */
class JsonDiagnosticParserTest {

    @Test
    fun fullRecordParsesAndSeverityMaps() {
        val e = JsonDiagnosticParser.parse(
            """{"severity":"error","code":"CJ1","message":"bad type","file":"/p/A.cajeta","line":10,"column":5}""")!!
        assertEquals(Diagnostic.Severity.ERROR, e.severity)
        assertEquals("CJ1", e.code)
        assertEquals("bad type", e.message)
        assertEquals("/p/A.cajeta", e.file)
        assertEquals(10, e.line)
        assertEquals(5, e.column)

        assertEquals(Diagnostic.Severity.WARNING,
            JsonDiagnosticParser.parse("""{"severity":"warning","message":"m"}""")!!.severity)
        assertEquals(Diagnostic.Severity.WEAK_WARNING,
            JsonDiagnosticParser.parse("""{"severity":"note","message":"m"}""")!!.severity)
    }

    @Test
    fun jsonNullsBecomeAbsentFields() {
        val d = JsonDiagnosticParser.parse(
            """{"severity":"error","code":null,"message":"m","file":null,"line":null,"column":null}""")!!
        assertNull(d.code)
        assertNull(d.file)
        assertNull(d.line)
        assertNull(d.column)
        assertEquals("m", d.message)
    }

    @Test
    fun nonJsonOrNonDiagnosticLineReturnsNull() {
        assertNull(JsonDiagnosticParser.parse("cajeta build: compiler exited 1"))
        assertNull(JsonDiagnosticParser.parse(""))
        assertNull(JsonDiagnosticParser.parse("{ this is not json"))
        // valid JSON object but no severity -> not a diagnostic
        assertNull(JsonDiagnosticParser.parse("""{"note":"hello"}"""))
    }

    @Test
    fun rfc8259EscapesDecode() {
        val d = JsonDiagnosticParser.parse(
            """{"severity":"error","message":"a\"b\\c\nd","file":"/p/with space/A.cajeta"}""")!!
        assertEquals("a\"b\\c\nd", d.message)
        assertEquals("/p/with space/A.cajeta", d.file)
    }
}
