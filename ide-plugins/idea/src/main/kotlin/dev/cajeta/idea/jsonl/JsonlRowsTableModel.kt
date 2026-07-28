package dev.cajeta.idea.jsonl

import javax.swing.table.AbstractTableModel

/**
 * Table model over the engine's columns, shared by the buildtool console panel
 * and the session-console wrapper: one leading `#` line column, then a cell per
 * derived column; a [JsonlRow.Raw] shows as its verbatim text in the first data
 * column, prefixed mixed lines (§2.1.6) show their prefix there too. Update
 * fires a structure change only when the columns actually changed, so burst
 * refreshes (§3.1.5) don't reset column widths per batch.
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

    override fun getRowCount() = rows.size
    override fun getColumnCount() = columns.size + 1
    override fun getColumnName(c: Int) = if (c == 0) "#" else columns[c - 1]

    override fun getValueAt(r: Int, c: Int): Any {
        val row = rows[r]
        if (c == 0) return row.lineNumber
        return when (row) {
            is JsonlRow.Record -> JsonlEngine.cell(row, columns[c - 1])
            is JsonlRow.Raw -> if (c == 1) row.text else ""   // raw text in first data column
        }
    }
}
