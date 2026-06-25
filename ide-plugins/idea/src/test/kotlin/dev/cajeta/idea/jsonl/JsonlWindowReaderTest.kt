package dev.cajeta.idea.jsonl

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * W-buildtool unit 10.1.1: the windowing core for the standalone `.jsonl` viewer
 * (spec §8.2.2). A large file is read in bounded windows over a lazy line source
 * — the reader must NOT pull the whole sequence to return a small window — and
 * renders rows identically to the shared §8 engine (§8.2.3).
 */
class JsonlWindowReaderTest {

    /** A lazy line source that counts how many lines were actually pulled, to
     *  prove the reader doesn't materialize the whole file. */
    private class CountingSource(val total: Int) {
        var pulled = 0
        fun seq(): Sequence<String> =
            generateSequence(1) { if (it >= total) null else it + 1 }
                .map { pulled++; """{"i":$it,"level":"info"}""" }
    }

    @Test
    fun returnsBoundedSliceWithoutReadingWholeFile() {
        val src = CountingSource(1_000_000)
        val w = JsonlWindowReader.read(src.seq(), startLine = 500, count = 10)
        assertEquals(500, w.startLine)
        assertEquals(10, w.rows.size)
        assertTrue("more lines follow", w.hasMore)
        // Laziness: pulled ~ startLine + count + 1 peek, NOWHERE near 1,000,000.
        assertTrue("read lazily, pulled=${src.pulled}", src.pulled <= 520)
        // window content is the right slice
        val first = w.rows.first() as JsonlRow.Record
        assertEquals("500", JsonlEngine.cell(first, "i"))
    }

    @Test
    fun hasMoreFalseAtEndAndColumnsDerived() {
        val src = CountingSource(30)
        val w = JsonlWindowReader.read(src.seq(), startLine = 25, count = 100)
        assertEquals(6, w.rows.size)          // lines 25..30
        assertFalse(w.hasMore)
        // columns match the engine's deterministic order (preferred first)
        assertEquals(listOf("level", "i"), w.columns)
    }

    @Test
    fun rowsRenderIdenticallyToEngineIncludingRawPassthrough() {
        val lines = sequenceOf(
            """{"level":"warn","msg":"a"}""",
            "not json at all",
            "",                               // blank carries nothing
            """{"level":"error","msg":"b"}""",
        )
        val w = JsonlWindowReader.read(lines, startLine = 1, count = 10)
        // blank dropped; raw passthrough kept (§7.2.4) with its physical line number
        assertEquals(3, w.rows.size)
        assertTrue(w.rows.any { it is JsonlRow.Raw && it.lineNumber == 2 })
        assertEquals(4, (w.rows.last() as JsonlRow.Record).lineNumber)
    }

    @Test
    fun startBeyondEndIsEmptyWindow() {
        val src = CountingSource(10)
        val w = JsonlWindowReader.read(src.seq(), startLine = 50, count = 10)
        assertTrue(w.rows.isEmpty())
        assertFalse(w.hasMore)
    }
}
