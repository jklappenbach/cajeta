package dev.cajeta.idea.jsonl

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * compiler-jsonl Unit 5 — the console dispatches on `kind` for compiler
 * records, and keeps working for everything else.
 *
 * The console renders TWO kinds of stream: the compiler's own (every record
 * self-describing since Unit 1) and arbitrary third-party JSONL, which has no
 * `kind` and never will — cajeta-logging emits
 * `{"ts":…,"level":"INFO","logger":…,"msg":…}`. So dispatch goes in FRONT of
 * the field probe rather than replacing it (spec 6.1.2).
 */
class JsonlCompilerRecordsTest {

    private fun row(line: String) =
        JsonlEngine.parseLine(1, line) as JsonlRow.Record

    @Test
    fun compilerRecordsExposeTheirKind() {
        assertEquals("diagnostic", row(
            """{"kind":"diagnostic","severity":"error","message":"m"}""").kind)
        assertEquals("log", row(
            """{"kind":"log","level":"debug","message":"[jit] collect"}""").kind)
        assertEquals("result", row("""{"kind":"result","status":"ok"}""").kind)
    }

    @Test
    fun thirdPartyRecordsHaveNoKind() {
        // The tour's actual shape.
        val r = row("""{"ts":1785530919951,"level":"INFO","logger":"orders.service","msg":"service started"}""")
        assertNull(r.kind)
        // ...and still level correctly, through the permanent fallback.
        assertEquals("info", r.level)
    }

    @Test
    fun diagnosticLevelsFromSeverityAndLogFromLevel() {
        assertEquals("error", row(
            """{"kind":"diagnostic","severity":"error","message":"m"}""").level)
        assertEquals("warning", row(
            """{"kind":"diagnostic","severity":"warning","message":"m"}""").level)
        assertEquals("debug", row(
            """{"kind":"log","level":"debug","message":"[jit] collect"}""").level)
    }

    // A failing build's terminal record has neither `level` nor `severity`, so
    // the field probe alone leaves it untinted — the ONE row a developer most
    // needs to see would render as ordinary text.
    @Test
    fun failingResultTintsAsAnError() {
        assertEquals(RowTint.ERROR,
            JsonlEngine.tintOf(row("""{"kind":"result","status":"error","message":"boom"}""")))
        assertEquals(RowTint.NORMAL,
            JsonlEngine.tintOf(row("""{"kind":"result","status":"ok"}""")))
    }

    @Test
    fun errorLogRecordsTintLikeDiagnostics() {
        assertEquals(RowTint.ERROR, JsonlEngine.tintOf(row(
            """{"kind":"log","level":"error","message":"cajeta jit: dependency resolution failed"}""")))
        assertEquals(RowTint.WARN, JsonlEngine.tintOf(row(
            """{"kind":"log","level":"warn","message":"native lib missing"}""")))
        assertEquals(RowTint.NORMAL, JsonlEngine.tintOf(row(
            """{"kind":"log","level":"debug","message":"[jit] collect"}""")))
    }

    // 5.1.3 — the level filter catches `log` records by their level, so the
    // narration Unit 3 structured is actually filterable.
    @Test
    fun levelFilterCatchesLogRecords() {
        val keep = JsonlEngine.atOrAboveLevel("warn")
        assertTrue(keep(row("""{"kind":"log","level":"error","message":"e"}""")))
        assertTrue(keep(row("""{"kind":"log","level":"warn","message":"w"}""")))
        assertTrue(!keep(row("""{"kind":"log","level":"debug","message":"d"}""")))
    }

    // A failing result must survive a level filter: it is the reason the run
    // ended, and hiding it behind "warn and above" would reproduce exactly the
    // silence this whole format exists to remove.
    @Test
    fun failingResultSurvivesALevelFilter() {
        val keep = JsonlEngine.atOrAboveLevel("warn")
        assertTrue(keep(row("""{"kind":"result","status":"error","message":"boom"}""")))
    }

    // The version banner is metadata, not data. It stays visible (the viewer
    // never silently drops a line) but must not be mistaken for a levelled row.
    @Test
    fun streamRecordIsNeutral() {
        val r = row("""{"kind":"stream","major":1,"minor":0,"producer":"cajeta 0.10.0"}""")
        assertEquals("stream", r.kind)
        assertNull(r.level)
        assertEquals(RowTint.NORMAL, JsonlEngine.tintOf(r))
    }
}
