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
}
