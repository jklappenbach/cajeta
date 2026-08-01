package dev.cajeta.idea.jsonl

import javax.swing.JCheckBoxMenuItem
import javax.swing.JMenuItem
import javax.swing.JPopupMenu
import javax.swing.JTable

/**
 * The Swing half of column selection and sizing (spec §3.1.7, §3.1.8), shared by
 * the three JSONL tables — the session console wrapper, the buildtool console
 * panel and the `.jsonl` editor — so all three behave identically. Thin by
 * design: every decision belongs to [JsonlColumns]; this only measures text and
 * builds menu items.
 */
object JsonlTableSupport {

    /** Slack around a cell's text so a content-sized column isn't flush. */
    const val CELL_PADDING = 16

    /**
     * Size columns to their widest content, growing only (§3.1.8) — no maximum.
     * Growing only keeps a streaming table steady: a batch of short rows must
     * not narrow a column mid-read, and a manual resize wider than the content
     * survives the next refresh. Call with [JTable.AUTO_RESIZE_OFF] set, or the
     * table will squeeze these widths back into the viewport.
     */
    fun applyWidths(table: JTable, columns: List<String>, tracked: JsonlColumns) {
        val model = table.columnModel
        if (model.columnCount == 0) return
        val metrics = table.getFontMetrics(table.font)
        fun grow(index: Int, text: String) {
            if (index >= model.columnCount) return
            val column = model.getColumn(index)
            val width = metrics.stringWidth(text) + CELL_PADDING
            if (column.preferredWidth < width) column.preferredWidth = width
        }
        grow(0, "999999")                       // the `#` line column
        if (columns.isEmpty()) {
            // Empty selection: the single data column IS the line text
            // (§3.1.7.3), so it sizes to the widest line rather than a stub.
            grow(1, tracked.widestLine())
            return
        }
        for ((i, name) in columns.withIndex()) {
            val widest = tracked.widestCell(name)
            grow(i + 1, if (widest.length > name.length) widest else name)
        }
    }

    /**
     * The column chooser as a plain Swing popup, built fresh at show time from
     * the live discovery list (§3.1.7.1) — a field the stream revealed a moment
     * ago is present without a reload. [onChange] re-renders the table.
     */
    fun fieldsPopup(tracked: JsonlColumns, onChange: () -> Unit): JPopupMenu {
        val menu = JPopupMenu("Fields")
        val fields = tracked.available()
        if (fields.isEmpty()) {
            menu.add(JMenuItem("No fields discovered yet").apply { isEnabled = false })
            return menu
        }
        for (field in fields) {
            menu.add(JCheckBoxMenuItem(field, tracked.isVisible(field)).apply {
                addActionListener {
                    tracked.setFieldVisible(field, isSelected)
                    onChange()
                }
            })
        }
        menu.addSeparator()
        menu.add(JMenuItem("Reset to Defaults").apply {
            addActionListener {
                tracked.resetToDefaults()
                onChange()
            }
        })
        return menu
    }
}
