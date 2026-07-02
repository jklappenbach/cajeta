package dev.cajeta.idea.lint

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * json-diagnostics U3: the lint tier turns the compiler's `--diag-format=json`
 * NDJSON into editor [Diagnostic]s at precise ranges (spec §4). Exercises the
 * pure parse/range logic; the spawn path is covered by a runIde smoke.
 */
class CajetacRunnerTest {

    private val buffer =
        "package p;\n" +                       // line 1
        "public final class C {\n" +           // line 2
        "    public static void main() {\n" +  // line 3
        "        NoSuchType z = null;\n" +      // line 4, `NoSuchType` starts at col 9
        "    }\n" +
        "}\n"

    @Test
    fun ndjsonErrorMapsToPreciseTokenRangeWithCodeAsRuleId() {
        val stderr =
            """{"severity":"error","code":"CJ_UNRESOLVED","message":"unresolved type 'NoSuchType'","file":"/tmp/x.cajeta","line":4,"column":9}"""
        val diags = CajetacRunner.parseDiagnostics(stderr, buffer)
        assertEquals(1, diags.size)
        val d = diags[0]
        assertEquals(Diagnostic.Severity.ERROR, d.severity)
        assertEquals("CJ_UNRESOLVED", d.ruleId)
        assertTrue(d.message.contains("NoSuchType"))
        // The range must cover the `NoSuchType` token at line 4, col 9.
        assertEquals("NoSuchType", buffer.substring(d.range.startOffset, d.range.endOffset))
    }

    @Test
    fun warningAndNoteSeveritiesCarryThrough() {
        val stderr = listOf(
            """{"severity":"warning","code":"W1","message":"unused","file":"/x","line":2,"column":1}""",
            """{"severity":"note","message":"fyi","file":null,"line":null,"column":null}""",
        ).joinToString("\n")
        val diags = CajetacRunner.parseDiagnostics(stderr, buffer)
        assertEquals(2, diags.size)
        assertEquals(Diagnostic.Severity.WARNING, diags[0].severity)
        assertEquals(Diagnostic.Severity.WEAK_WARNING, diags[1].severity)
    }

    @Test
    fun nonJsonConsoleLinesAreIgnored() {
        val stderr = "Compiling module p…\ncajeta: done\n"
        assertTrue(CajetacRunner.parseDiagnostics(stderr, buffer).isEmpty())
    }
}
