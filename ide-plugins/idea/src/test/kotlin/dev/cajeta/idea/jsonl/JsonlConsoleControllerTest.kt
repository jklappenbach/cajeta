package dev.cajeta.idea.jsonl

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * W-buildtool unit 9.1.1: the plain-JVM console controller (spec §7). Streams
 * process output incrementally into the §8 render model, toggles structured/raw,
 * and applies level/field filters — all without `com.intellij.*`, so the
 * streaming + filtering logic is unit-tested independent of the console pane.
 */
class JsonlConsoleControllerTest {

    // §6.2.1 / §7.2.1: output arrives in chunks that need not split on line
    // boundaries; a record only materializes once its line is complete.
    @Test
    fun streamsPartialLinesAndMaterializesOnLineComplete() {
        val c = JsonlConsoleController(structuredByDefault = true)
        c.append("""{"level":"info",""")             // half a line — not yet a row
        assertTrue("incomplete line yields no record", c.visibleRows().none { it is JsonlRow.Record })
        c.append(""""msg":"hello"}""" + "\n")        // completes the line
        val rows = c.visibleRows()
        assertEquals(1, rows.size)
        val rec = rows.single() as JsonlRow.Record
        assertEquals("hello", JsonlEngine.cell(rec, "msg"))
    }

    // §7.2.2: structured view is filterable; raw view is the verbatim stream.
    @Test
    fun rawModeReturnsVerbatimStreamStructuredReturnsRows() {
        val c = JsonlConsoleController(structuredByDefault = true)
        c.append("""{"level":"info","msg":"a"}""" + "\n")
        c.append("plain interleaved text\n")
        // structured: 1 record + 1 raw passthrough (§7.2.4 never dropped)
        assertEquals(2, c.visibleRows().size)
        assertTrue(c.visibleRows().any { it is JsonlRow.Raw })

        c.setStructured(false)
        assertFalse(c.isStructured)
        assertEquals(
            "{\"level\":\"info\",\"msg\":\"a\"}\nplain interleaved text\n",
            c.rawText(),
        )
    }

    // §7.2.2: a level filter hides below-threshold records but keeps raw lines.
    @Test
    fun levelFilterHidesLowRecordsButKeepsRaw() {
        val c = JsonlConsoleController(structuredByDefault = true)
        c.append("""{"level":"info","msg":"lo"}""" + "\n")
        c.append("""{"level":"error","msg":"hi"}""" + "\n")
        c.append("interleaved\n")
        c.setLevelFilter("warn")
        val rows = c.visibleRows()
        // only the error record + the raw passthrough survive
        assertEquals(2, rows.size)
        assertTrue(rows.any { it is JsonlRow.Record && JsonlEngine.cell(it, "msg") == "hi" })
        assertTrue(rows.any { it is JsonlRow.Raw })

        c.clearFilter()
        assertEquals(3, c.visibleRows().size)
    }

    // §7.1: columns are derived deterministically across all streamed records,
    // accruing as new fields appear in later chunks.
    @Test
    fun columnsAccrueDeterministicallyAcrossChunks() {
        val c = JsonlConsoleController(structuredByDefault = true)
        c.append("""{"level":"info","msg":"a"}""" + "\n")
        c.append("""{"level":"warn","action":"link","msg":"b"}""" + "\n")
        // preferred order (level, action, msg) regardless of first appearance
        assertEquals(listOf("level", "action", "msg"), c.model().columns)
    }

    // json-viewer §3.1.7 (7.1.6): streaming feeds column discovery, so the
    // chooser's list fills as the run goes. `model()` keeps the FULL
    // deterministic order — the chooser needs every field, not the shown ones —
    // while `visibleColumns()` is what the table renders.
    @Test
    fun streamingFeedsColumnDiscoveryAndSelection() {
        val c = JsonlConsoleController(structuredByDefault = true)
        c.columns.defaultCount = 2
        c.append("""{"level":"info","msg":"a"}""" + "\n")
        c.append("""{"level":"warn","action":"link","msg":"b","file":"A.cajeta"}""" + "\n")

        assertEquals(listOf("level", "action", "msg", "file"), c.model().columns)
        assertEquals(listOf("level", "action"), c.visibleColumns())

        c.columns.setFieldVisible("file", true)
        assertEquals(listOf("level", "action", "file"), c.visibleColumns())
    }

    // A field filter narrows to matching records; raw passthrough still shown.
    @Test
    fun fieldFilterNarrowsToMatchingRecords() {
        val c = JsonlConsoleController(structuredByDefault = true)
        c.append("""{"action":"build","msg":"a"}""" + "\n")
        c.append("""{"action":"test","msg":"b"}""" + "\n")
        c.setFieldFilter("action", "build")
        val recs = c.visibleRows().filterIsInstance<JsonlRow.Record>()
        assertEquals(1, recs.size)
        assertEquals("a", JsonlEngine.cell(recs.single(), "msg"))
    }

    // --- json-viewer unit 2 (spec §3.1.4, §3.1.5): burst order/completeness ---

    // A 10k-line burst delivered in ragged chunks (line boundaries ignored, the
    // batched-append pattern polls the model between chunks) keeps every row, in
    // stream order, with nothing dropped or reordered.
    @Test
    fun burstOf10kLinesPreservesOrderAndCompleteness() {
        val c = JsonlConsoleController(structuredByDefault = true)
        val total = 10_000
        val text = buildString {
            for (i in 1..total) {
                if (i % 100 == 0) append("plain progress line $i\n")   // interleaved raw
                else append("""{"level":"info","message":"m$i"}""" + "\n")
            }
        }
        var i = 0
        while (i < text.length) {                     // 733 splits lines mid-way
            val end = minOf(text.length, i + 733)
            c.append(text.substring(i, end))
            c.visibleRows()                           // a batched refresh per chunk
            i = end
        }
        val rows = c.visibleRows()
        assertEquals(total, rows.size)
        assertEquals((1..total).toList(), rows.map { it.lineNumber })   // stream order
        assertEquals(total / 100, rows.count { it is JsonlRow.Raw })
        // line 10000 is a raw progress line, so the last record is m9999
        assertEquals(
            "m${total - 1}",
            JsonlEngine.cell(rows.last { it is JsonlRow.Record } as JsonlRow.Record, "message"),
        )
    }

    // --- json-viewer unit 2 (spec §3.1.3): level→tint mapping for row coloring ---

    @Test
    fun levelMapsToRowTintForErrorWarnAndDefault() {
        fun rec(level: String) = JsonlEngine.parseLine(1, """{"level":"$level"}""") as JsonlRow.Record
        assertEquals(RowTint.ERROR, JsonlEngine.tintOf(rec("error")))
        assertEquals(RowTint.ERROR, JsonlEngine.tintOf(rec("FATAL")))
        assertEquals(RowTint.WARN, JsonlEngine.tintOf(rec("warn")))
        assertEquals(RowTint.WARN, JsonlEngine.tintOf(rec("warning")))
        assertEquals(RowTint.NORMAL, JsonlEngine.tintOf(rec("info")))
        assertEquals(RowTint.NORMAL, JsonlEngine.tintOf(JsonlRow.Raw(1, "plain")))
    }

    // --- json-viewer unit 2 (spec §3.2.1, 1.3.3): in-place toggle loses nothing ---

    @Test
    fun togglingStructuredRawMidStreamLosesNothingOnEitherCard() {
        val c = JsonlConsoleController(structuredByDefault = true)
        c.append("""{"level":"info","message":"one"}""" + "\n")
        c.setStructured(false)
        c.append("plain two\n")
        c.setStructured(true)
        c.append("""{"level":"warn","message":"three"}""" + "\n")
        // structured card: every line, in order, regardless of when toggles happened
        val rows = c.visibleRows()
        assertEquals(listOf(1, 2, 3), rows.map { it.lineNumber })
        assertTrue(rows[1] is JsonlRow.Raw)
        // raw card: the verbatim stream, byte-complete
        assertEquals(
            "{\"level\":\"info\",\"message\":\"one\"}\nplain two\n{\"level\":\"warn\",\"message\":\"three\"}\n",
            c.rawText(),
        )
    }
}
