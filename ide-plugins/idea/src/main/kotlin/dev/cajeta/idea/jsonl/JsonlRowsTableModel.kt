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
    private var columns: List<String> = emptyList()
    private var rows: List<JsonlRow> = emptyList()

    fun update(columns: List<String>, rows: List<JsonlRow>) {
        val structureChanged = columns != this.columns
        this.columns = columns
        this.rows = rows
        if (structureChanged) fireTableStructureChanged() else fireTableDataChanged()
    }

    fun rowAt(r: Int): JsonlRow? = rows.getOrNull(r)

    /** The row's whole line, for a tooltip: a raw passthrough doesn't widen its
     *  column (§3.1.8.1), so hovering is how a long one is read in place. */
    fun lineTextAt(r: Int): String = rows.getOrNull(r)?.let { lineTextOf(it) } ?: ""

    override fun getRowCount() = rows.size
    override fun getColumnCount() = if (columns.isEmpty()) 2 else columns.size + 1
    override fun getColumnName(c: Int) =
        if (c == 0) "#" else if (columns.isEmpty()) "line" else columns[c - 1]

    override fun getValueAt(r: Int, c: Int): Any {
        val row = rows[r]
        if (c == 0) return row.lineNumber
        if (columns.isEmpty()) return lineTextOf(row)
        return when (row) {
            is JsonlRow.Record -> JsonlEngine.cell(row, columns[c - 1])
            is JsonlRow.Raw -> if (c == 1) row.text else ""   // raw text in first data column
        }
    }

    private fun lineTextOf(row: JsonlRow): String = when (row) {
        is JsonlRow.Record -> row.raw
        is JsonlRow.Raw -> row.text
    }
}
