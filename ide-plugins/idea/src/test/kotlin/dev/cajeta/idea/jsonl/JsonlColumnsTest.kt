package dev.cajeta.idea.jsonl

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * json-viewer Unit 7 (spec §3.1.7, §3.1.8): the table opens on a SUBSET of the
 * discovered fields, the chooser's list grows with the stream, an explicit
 * choice pins the selection, and column widths track the widest record cell
 * without a ceiling. All of it pure — the Swing surfaces only render what this
 * decides.
 */
class JsonlColumnsTest {

    private fun record(line: Int, json: String): JsonlRow =
        JsonlEngine.parseLine(line, json)!!

    // --- 7.1.1 defaults ---------------------------------------------------

    @Test
    fun visibleStartsAsASubsetOfWhatWasDiscovered() {
        val cols = JsonlColumns(defaultCount = 3)
        cols.observe(record(1, """{"timestamp":"t","level":"info","action":"a","file":"f","line":7,"extra":"e"}"""))

        assertEquals(
            "every discovered field is offered",
            listOf("timestamp", "level", "action", "file", "line", "extra"),
            cols.available(),
        )
        assertEquals(
            "only the first N of the deterministic order are shown",
            listOf("timestamp", "level", "action"),
            cols.visible(),
        )
    }

    @Test
    fun preferredKeysLeadTheOrderRegardlessOfEncounterOrder() {
        val cols = JsonlColumns(defaultCount = 2)
        cols.observe(record(1, """{"zeta":1,"message":"m","alpha":2,"level":"warn"}"""))

        assertEquals(listOf("level", "message", "zeta", "alpha"), cols.available())
        assertEquals(listOf("level", "message"), cols.visible())
    }

    @Test
    fun fewerFieldsThanTheDefaultShowsThemAll() {
        val cols = JsonlColumns(defaultCount = 5)
        cols.observe(record(1, """{"level":"info","msg":"hi"}"""))
        assertEquals(listOf("level", "msg"), cols.visible())
    }

    // --- 7.1.2 discovery --------------------------------------------------

    @Test
    fun laterRecordsAddFieldsWithoutDisturbingExistingOrder() {
        val cols = JsonlColumns(defaultCount = 2)
        cols.observe(record(1, """{"level":"info","action":"start"}"""))
        cols.observe(record(2, """{"level":"info","fresh":"x"}"""))

        assertEquals(listOf("level", "action", "fresh"), cols.available())
    }

    @Test
    fun availableNeverShrinks() {
        val cols = JsonlColumns(defaultCount = 2)
        cols.observe(record(1, """{"level":"info","only-here":1}"""))
        cols.observe(record(2, """{"level":"info"}"""))
        cols.observe(JsonlRow.Raw(3, "a plain line"))

        assertEquals(listOf("level", "only-here"), cols.available())
    }

    @Test
    fun rawRowsContributeNoFields() {
        val cols = JsonlColumns(defaultCount = 3)
        cols.observe(JsonlRow.Raw(1, """not json {but bracey"""))
        assertTrue(cols.available().isEmpty())
        assertTrue(cols.visible().isEmpty())
    }

    // --- 7.1.3 pinning ----------------------------------------------------

    @Test
    fun defaultsAreRecomputedUntilTheReaderChooses() {
        val cols = JsonlColumns(defaultCount = 2)
        // A metadata-only opening line must not fix a poor column set.
        cols.observe(record(1, """{"kind":"stream","major":1}"""))
        assertEquals(listOf("kind", "major"), cols.visible())

        cols.observe(record(2, """{"level":"error","message":"boom","kind":"log"}"""))
        assertEquals("still unpinned, so the default improves as the shape is revealed",
            listOf("level", "message"), cols.visible())
    }

    @Test
    fun theFirstTogglePinsTheSelection() {
        val cols = JsonlColumns(defaultCount = 2)
        cols.observe(record(1, """{"level":"info","message":"m","file":"f"}"""))
        assertFalse(cols.isPinned())

        cols.setFieldVisible("file", true)
        assertTrue(cols.isPinned())
        assertEquals(listOf("level", "message", "file"), cols.visible())

        // A field discovered after the choice is offered but stays off.
        cols.observe(record(2, """{"level":"info","latecomer":1}"""))
        assertTrue("latecomer" in cols.available())
        assertEquals(listOf("level", "message", "file"), cols.visible())
    }

    @Test
    fun visibleKeepsTheDeterministicOrderNotTheToggleOrder() {
        val cols = JsonlColumns(defaultCount = 1)
        cols.observe(record(1, """{"level":"info","alpha":1,"message":"m"}"""))
        cols.setFieldVisible("alpha", true)
        cols.setFieldVisible("message", true)

        assertEquals(listOf("level", "message", "alpha"), cols.visible())
    }

    @Test
    fun hidingAFieldRemovesItButLeavesItAvailable() {
        val cols = JsonlColumns(defaultCount = 3)
        cols.observe(record(1, """{"level":"info","message":"m","file":"f"}"""))
        cols.setFieldVisible("message", false)

        assertEquals(listOf("level", "file"), cols.visible())
        assertTrue("message" in cols.available())
    }

    @Test
    fun resetRestoresDefaultsAndUnpins() {
        val cols = JsonlColumns(defaultCount = 2)
        cols.observe(record(1, """{"level":"info","message":"m","file":"f"}"""))
        cols.setFieldVisible("message", false)
        cols.resetToDefaults()

        assertFalse(cols.isPinned())
        assertEquals(listOf("level", "message"), cols.visible())
        // ...and tracking resumes: a newly revealed preferred key can take a slot.
        cols.observe(record(2, """{"timestamp":"t","level":"info"}"""))
        assertEquals(listOf("timestamp", "level"), cols.visible())
    }

    // --- 7.1.4 the empty selection ----------------------------------------

    @Test
    fun everythingCanBeDeselected() {
        val cols = JsonlColumns(defaultCount = 2)
        cols.observe(record(1, """{"level":"info","message":"m"}"""))
        cols.setFieldVisible("level", false)
        cols.setFieldVisible("message", false)

        assertTrue(cols.visible().isEmpty())
        assertTrue(cols.isPinned())
    }

    @Test
    fun anEmptySelectionStillRendersRawRows() {
        // §3.1.7.3: with no fields chosen the model degenerates to one
        // line-text column so the verbatim stream is never hidden.
        val model = JsonlRowsTableModel()
        model.update(emptyList(), listOf(JsonlRow.Raw(1, "plain output"), record(2, """{"level":"info"}""")))

        assertEquals("# plus the line-text column", 2, model.columnCount)
        assertEquals("plain output", model.getValueAt(0, 1))
        assertEquals("a record falls back to its raw line",
            """{"level":"info"}""", model.getValueAt(1, 1))
    }

    // A raw line is NOT a field value, and must never be presented as one.
    //
    // getValueAt put a raw row's whole text at c == 1 — "the first data
    // column", whatever it happened to be NAMED. With the engine's default
    // order (timestamp, time, ts, level, severity, message, …) a stream
    // without a timestamp field puts `level` first, so every plain line in a
    // mixed console rendered under a header claiming it was a severity
    // (Julian, 2026-08-31, for a whole debug run). The existing raw-row tests
    // asserted only that such rows stay VISIBLE and don't widen a column —
    // never that their text lands somewhere truthful, which is how this
    // survived.
    @Test
    fun aRawRowNeverOccupiesANamedFieldColumn() {
        val model = JsonlRowsTableModel()
        model.update(
            listOf("level", "message"),
            listOf(JsonlRow.Raw(1, "warning: [plain-return-yields-title] ...")),
        )

        assertEquals("", model.getValueAt(0, 1))   // the `level` column
        assertEquals("", model.getValueAt(0, 2))   // the `message` column
    }

    // …but it must still be readable: nothing is dropped (§3.1.4). The text is
    // offered as a row-level span rather than as a cell, which is what the
    // table paints across the data columns.
    @Test
    fun aRawRowOffersItsTextAsARowSpan() {
        val model = JsonlRowsTableModel()
        val text = "warning: [plain-return-yields-title] ..."
        model.update(listOf("level", "message"), listOf(JsonlRow.Raw(1, text)))

        assertTrue(model.isRawRow(0))
        assertEquals(text, model.rawTextAt(0))
    }

    @Test
    fun aRecordRowIsNotASpanAndKeepsItsCells() {
        val model = JsonlRowsTableModel()
        model.update(listOf("level", "message"),
                     listOf(record(1, """{"level":"warn","message":"hi"}""")))

        assertTrue(!model.isRawRow(0))
        assertEquals(null, model.rawTextAt(0))
        assertEquals("warn", model.getValueAt(0, 1))
        assertEquals("hi", model.getValueAt(0, 2))
    }

    // A column IS its key — there is no ordinal (Julian's call, 2026-08-31).
    // The line column used to be a magic `c == 0` with every field lookup
    // spelled `columns[c - 1]`, and that arithmetic was repeated in the model,
    // the width pass and the renderers. Three copies of one off-by-one is how
    // two sides drift into self-consistent disagreement, which is the shape of
    // the xref column bug that opened this workstream.
    @Test
    fun everyColumnIsIdentifiedByItsKeyIncludingTheReservedOnes() {
        val model = JsonlRowsTableModel()
        model.update(listOf("level", "message"), listOf(record(1, """{"level":"warn"}""")))

        assertEquals(
            listOf(JsonlRowsTableModel.LINE_KEY, "level", "message"),
            model.keys(),
        )
        assertEquals(JsonlRowsTableModel.LINE_KEY, model.keyAt(0))
        assertEquals("level", model.keyAt(1))
        assertEquals("message", model.keyAt(2))
        assertEquals("", model.keyAt(99))
        // Headers stay human: the reserved key is displayed as `#`.
        assertEquals("#", model.getColumnName(0))
        assertEquals("level", model.getColumnName(1))
    }

    @Test
    fun anEmptySelectionIsTwoReservedKeys() {
        val model = JsonlRowsTableModel()
        model.update(emptyList(), listOf(JsonlRow.Raw(1, "plain output")))

        assertEquals(
            listOf(JsonlRowsTableModel.LINE_KEY, JsonlRowsTableModel.LINE_TEXT_KEY),
            model.keys(),
        )
        assertEquals("line", model.getColumnName(1))
    }

    // The reserved keys cannot be shadowed by a real field: they are
    // \u0000-prefixed and every emitter produces printable keys. A record
    // carrying a literal "level" is still read from "level", not from a
    // reserved slot.
    @Test
    fun aReservedKeyCannotCollideWithAFieldName() {
        val model = JsonlRowsTableModel()
        model.update(listOf("#", "line"),
                     listOf(record(1, """{"#":"hash","line":"text"}""")))

        assertEquals(JsonlRowsTableModel.LINE_KEY, model.keyAt(0))
        assertEquals("#", model.keyAt(1))
        assertEquals(1, model.getValueAt(0, 0))      // the real line number
        assertEquals("hash", model.getValueAt(0, 1)) // the field named "#"
        assertEquals("text", model.getValueAt(0, 2))
    }

    // --- 7.1.5 width tracking ---------------------------------------------

    @Test
    fun theWidestRecordCellPerFieldIsRetained() {
        val cols = JsonlColumns(defaultCount = 5)
        cols.observe(record(1, """{"message":"short"}"""))
        cols.observe(record(2, """{"message":"a considerably longer message"}"""))
        cols.observe(record(3, """{"message":"mid length"}"""))

        assertEquals("grow-only: a later short cell never narrows the column",
            "a considerably longer message", cols.widestCell("message"))
    }

    @Test
    fun widthIgnoresRawRows() {
        val cols = JsonlColumns(defaultCount = 5)
        cols.observe(record(1, """{"level":"info"}"""))
        cols.observe(JsonlRow.Raw(2, "x".repeat(500)))

        assertEquals("info", cols.widestCell("level"))
        assertEquals("", cols.widestCell("nothing-here"))
    }

    @Test
    fun nestedValuesMeasureAsTheirRenderedText() {
        val cols = JsonlColumns(defaultCount = 5)
        cols.observe(record(1, """{"span":{"file":"A.cajeta","line":12}}"""))
        assertEquals(JsonlEngine.cell(
            record(1, """{"span":{"file":"A.cajeta","line":12}}""") as JsonlRow.Record, "span"),
            cols.widestCell("span"))
    }
}
