package dev.cajeta.idea.jsonl

import javax.swing.table.AbstractTableModel

/**
 * Table model over the selected columns ([JsonlColumns]), shared by the
 * buildtool console panel and the session-console wrapper: one leading `#` line
 * column, then a cell per selected column; a [JsonlRow.Raw] shows as its
 * verbatim text in the first data column, prefixed mixed lines (§2.1.6) show
 * their prefix there too. Update fires a structure change only when the columns
 * actually changed, so burst refreshes (§3.1.5) don't reset column widths per
 * batch.
 *
 * With NO column selected the table degenerates to a single line-text column
 * (spec §3.1.7.3): deselecting everything is a legal way to ask for the plain
 * stream, and the verbatim rows must never become invisible (§3.1.4).
 */
class JsonlRowsTableModel : AbstractTableModel() {

    companion object {
        /** The leading line-number column. Reserved keys are `\u0000`-prefixed
         *  so no JSON key can collide: emitters produce printable keys. */
        const val LINE_KEY = "\u0000#"
        /** The single line-text column shown when nothing is selected
         *  (§3.1.7.3). */
        const val LINE_TEXT_KEY = "\u0000line"
    }

    // Every column is identified by its KEY, including the two reserved ones.
    // Previously the line column was a magic `c == 0` and every field lookup
    // was `columns[c - 1]`, arithmetic repeated across this model, the width
    // pass and the renderers — the same species as the xref column mismatch
    // that opened this workstream: two sides each self-consistently
    // disagreeing about an index. There is no ordinal now; a column IS its
    // key, which is also what the saved layout has always persisted, so
    // nothing about the stored format changes.
    private var keys: List<String> = listOf(LINE_KEY, LINE_TEXT_KEY)
    private var rows: List<JsonlRow> = emptyList()

    fun update(columns: List<String>, rows: List<JsonlRow>) {
        val next =
            if (columns.isEmpty()) listOf(LINE_KEY, LINE_TEXT_KEY)
            else listOf(LINE_KEY) + columns
        val structureChanged = next != keys
        keys = next
        this.rows = rows
        if (structureChanged) fireTableStructureChanged() else fireTableDataChanged()
    }

    /** The key rendered by view column [c], or "" when out of range. */
    fun keyAt(c: Int): String = keys.getOrElse(c) { "" }

    /** Every column's key, in display order. */
    fun keys(): List<String> = keys

    fun rowAt(r: Int): JsonlRow? = rows.getOrNull(r)

    /** The row's whole line, for a tooltip: a raw passthrough doesn't widen its
     *  column (§3.1.8.1), so hovering is how a long one is read in place. */
    fun lineTextAt(r: Int): String = rows.getOrNull(r)?.let { lineTextOf(it) } ?: ""

    override fun getRowCount() = rows.size
    override fun getColumnCount() = keys.size
    override fun getColumnName(c: Int) = when (keyAt(c)) {
        LINE_KEY -> "#"
        LINE_TEXT_KEY -> "line"
        else -> keyAt(c)
    }

    override fun getValueAt(r: Int, c: Int): Any {
        val row = rows[r]
        val key = keyAt(c)
        if (key == LINE_KEY) return row.lineNumber
        if (key == LINE_TEXT_KEY) return lineTextOf(row)
        return when (row) {
            is JsonlRow.Record -> JsonlEngine.cell(row, key)
            // A raw line is not a field value. It used to go in `c == 1` —
            // "the first data column" — which is a POSITION, so it wore
            // whatever header happened to be there: with no timestamp field in
            // the stream that is `level`, and every plain line in a mixed
            // console read as a severity. The text is offered as a row span
            // instead ([rawTextAt]), which the table paints across the data
            // columns; no named column ever claims it.
            is JsonlRow.Raw -> ""
        }
    }

    /** True when row [r] is an unstructured passthrough line rather than a
     *  record — it has no cells, only [rawTextAt]. */
    fun isRawRow(r: Int): Boolean = rows.getOrNull(r) is JsonlRow.Raw

    /** The span text for a raw row, or null for a record. Kept separate from
     *  [lineTextAt], which answers the tooltip question for EVERY row. */
    fun rawTextAt(r: Int): String? = (rows.getOrNull(r) as? JsonlRow.Raw)?.text

    private fun lineTextOf(row: JsonlRow): String = when (row) {
        is JsonlRow.Record -> row.raw
        is JsonlRow.Raw -> row.text
    }
}
