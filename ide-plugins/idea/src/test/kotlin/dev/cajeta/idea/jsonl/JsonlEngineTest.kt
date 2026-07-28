package dev.cajeta.idea.jsonl

import dev.cajeta.idea.debugger.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * W-buildtool unit 8: the shared JSONL render engine (spec §7.1, §8.3). Columns
 * deterministic (preferred-first), every non-empty line preserved (non-JSON →
 * raw passthrough), level/field filters correct with raw rows always surviving.
 */
class JsonlEngineTest {

    private val sample = """
        {"timestamp":"t1","level":"info","action":"build","message":"start"}
        plain interleaved text, not json
        {"level":"warn","message":"slow","extra":{"ms":42}}
        {"level":"error","message":"boom"}
    """.trimIndent()

    @Test
    fun parsesRecordsAndRawPassthroughNeverDropping() {
        val model = JsonlEngine.parse(sample)
        assertEquals(4, model.rows.size)                       // no line dropped
        assertTrue(model.rows[1] is JsonlRow.Raw)              // the plain-text line
        assertEquals("plain interleaved text, not json", (model.rows[1] as JsonlRow.Raw).text)
        assertEquals(3, model.rows.count { it is JsonlRow.Record })
        // line numbers preserved: raw text is line 2, the warn record line 3.
        assertEquals(2, model.rows[1].lineNumber)
        assertEquals(3, model.rows[2].lineNumber)
    }

    @Test
    fun derivesDeterministicPreferredFirstColumns() {
        val model = JsonlEngine.parse(sample)
        // timestamp, level, action, message are preferred (in that order); extra
        // is a non-preferred key appended in first-appearance order.
        assertEquals(listOf("timestamp", "level", "action", "message", "extra"), model.columns)
    }

    @Test
    fun levelFilterKeepsAtOrAboveAndPassesRawThrough() {
        val rows = JsonlEngine.parse(sample).rows
        val warnPlus = rows.filter(JsonlEngine.atOrAboveLevel("warn"))
        // info dropped; warn + error kept; the raw line always survives.
        assertTrue(warnPlus.any { it is JsonlRow.Raw })
        val levels = warnPlus.filterIsInstance<JsonlRow.Record>().mapNotNull { it.level }
        assertEquals(listOf("warn", "error"), levels)
    }

    @Test
    fun fieldFilterMatchesRecordsAndPassesRaw() {
        val rows = JsonlEngine.parse(sample).rows
        val onlyBuild = rows.filter(JsonlEngine.fieldEquals("action", "build"))
        // one record has action=build; the raw row passes through.
        assertEquals(1, onlyBuild.filterIsInstance<JsonlRow.Record>().size)
        assertTrue(onlyBuild.any { it is JsonlRow.Raw })
    }

    @Test
    fun cellRendersScalarsAndCompactsNested() {
        val rec = JsonlEngine.parse(sample).rows
            .filterIsInstance<JsonlRow.Record>()
            .first { it.fields.containsKey("extra") }
        assertEquals("slow", JsonlEngine.cell(rec, "message"))
        assertEquals("", JsonlEngine.cell(rec, "action"))       // absent -> empty
        // nested object compacted to one cell (expandable in the UI).
        assertEquals("""{"ms":42}""", JsonlEngine.cell(rec, "extra"))
    }

    @Test
    fun recordExposesLevelFromLevelOrSeverity() {
        val model = JsonlEngine.parse("""{"severity":"WARN","message":"x"}""")
        val rec = model.rows.single() as JsonlRow.Record
        assertEquals("warn", rec.level)
        assertEquals(Json.Str("x"), rec.fields["message"])
    }

    // --- json-viewer unit 1 (spec §2.1.6): mixed prefix/ANSI-tolerant lines ---

    @Test
    fun prefixedLineRendersPrefixVerbatimAndStructuredTail() {
        val line = """2026-07-28 12:00:01 INFO {"level":"info","message":"listening"}"""
        val row = JsonlEngine.parseLine(1, line) as JsonlRow.Record
        assertEquals("2026-07-28 12:00:01 INFO ", row.prefix)
        assertEquals("listening", JsonlEngine.cell(row, "message"))
        assertEquals(line, row.raw)   // the original line is preserved untouched
    }

    @Test
    fun ansiEscapesAreToleratedAnywhere() {
        val green = "\u001B[32m"
        val reset = "\u001B[0m"
        val row = JsonlEngine.parseLine(1, """$green{"level":"info",$reset"message":"ok"}$reset""")
        assertTrue(row is JsonlRow.Record)
        assertEquals("ok", JsonlEngine.cell(row as JsonlRow.Record, "message"))
        // an ANSI-colored prefix before a JSON tail: prefix keeps its text, not the escapes
        val mixed = JsonlEngine.parseLine(2, "${green}INFO$reset {\"a\":1}") as JsonlRow.Record
        assertEquals("INFO ", mixed.prefix)
    }

    @Test
    fun plainTextWithBracesButNoJsonStaysRaw() {
        assertTrue(JsonlEngine.parseLine(1, "if (x) { launch(); }") is JsonlRow.Raw)
        assertTrue(JsonlEngine.parseLine(2, "no braces at all") is JsonlRow.Raw)
    }

    @Test
    fun pureJsonRecordHasEmptyPrefix() {
        val row = JsonlEngine.parseLine(1, """{"level":"info"}""") as JsonlRow.Record
        assertEquals("", row.prefix)
    }
}
