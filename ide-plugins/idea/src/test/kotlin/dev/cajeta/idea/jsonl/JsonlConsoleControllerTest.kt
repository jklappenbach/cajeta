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
}
