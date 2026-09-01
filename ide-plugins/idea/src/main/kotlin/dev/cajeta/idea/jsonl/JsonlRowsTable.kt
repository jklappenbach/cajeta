package dev.cajeta.idea.jsonl

import com.intellij.ui.table.JBTable
import java.awt.Component
import java.awt.Rectangle
import javax.swing.table.TableCellRenderer

/**
 * The table for [JsonlRowsTableModel], shared by every JSONL surface (console,
 * session wrapper, standalone editor, doc editor).
 *
 * Its one job beyond JBTable: an unstructured passthrough line is painted as a
 * single span across the data columns instead of being stuffed into whichever
 * column happens to be first. Putting it in a field column made the header lie
 * — with no timestamp in the stream the first column is `level`, so a whole
 * debug run's plain output read as severities (Julian, 2026-08-31). Nothing is
 * dropped (§3.1.4); it is simply not presented as a field value.
 *
 * The `#` line column (0) is never spanned over — the line number stays
 * readable, which is what makes a raw row locatable in the stream.
 */
class JsonlRowsTable(private val rows: JsonlRowsTableModel) : JBTable(rows) {

    private fun isSpanRow(viewRow: Int): Boolean =
        viewRow >= 0 && rows.isRawRow(convertRowIndexToModel(viewRow))

    override fun getCellRect(row: Int, column: Int, includeSpacing: Boolean): Rectangle {
        val rect = super.getCellRect(row, column, includeSpacing)
        if (rows.keyAt(column) == JsonlRowsTableModel.LINE_KEY
                || columnCount <= 1 || !isSpanRow(row)) return rect
        val first = super.getCellRect(row, 1, includeSpacing)
        val last = super.getCellRect(row, columnCount - 1, includeSpacing)
        return Rectangle(first.x, rect.y, (last.x + last.width) - first.x, rect.height)
    }

    override fun prepareRenderer(
        renderer: TableCellRenderer,
        row: Int,
        column: Int,
    ): Component {
        val component = super.prepareRenderer(renderer, row, column)
        if (rows.keyAt(column) != JsonlRowsTableModel.LINE_KEY && isSpanRow(row)) {
            // Only the first data column carries the text; the rest render
            // empty and are painted over by the spanned rect above.
            val text = if (column == 1) rows.rawTextAt(convertRowIndexToModel(row)) ?: "" else ""
            (component as? javax.swing.JLabel)?.text = text
        }
        return component
    }
}
