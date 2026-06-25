package dev.cajeta.idea.jsonl

import com.intellij.openapi.fileEditor.FileEditor
import com.intellij.openapi.fileEditor.FileEditorState
import com.intellij.openapi.util.UserDataHolderBase
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.ui.components.JBScrollPane
import com.intellij.ui.table.JBTable
import java.beans.PropertyChangeListener
import javax.swing.JButton
import javax.swing.JComponent
import javax.swing.JLabel
import javax.swing.JPanel
import javax.swing.JToolBar
import javax.swing.table.AbstractTableModel
import java.awt.BorderLayout

/**
 * The structured tab for a `.jsonl` file (spec §8): a paged table over
 * [JsonlWindowReader], so even a multi-hundred-MB file opens responsively — only
 * one [PAGE] window is read at a time (§8.2.2). Cells render through the shared
 * §8 [JsonlEngine], matching the console (§8.2.3). Read-only.
 */
class JsonlStructuredEditor(private val file: VirtualFile) : UserDataHolderBase(), FileEditor {

    private val tableModel = WindowTableModel()
    private val table = JBTable(tableModel)
    private val status = JLabel()
    private val prev = JButton("◀ Prev")
    private val next = JButton("Next ▶")
    private val panel = JPanel(BorderLayout())

    private var startLine = 1

    init {
        val bar = JToolBar().apply {
            isFloatable = false
            add(prev.apply { addActionListener { page(-PAGE) } })
            add(next.apply { addActionListener { page(PAGE) } })
            add(status)
        }
        panel.add(bar, BorderLayout.NORTH)
        panel.add(JBScrollPane(table), BorderLayout.CENTER)
        load()
    }

    private fun page(delta: Int) {
        startLine = (startLine + delta).coerceAtLeast(1)
        load()
    }

    private fun load() {
        val window = readWindow(startLine, PAGE)
        tableModel.update(window.columns, window.rows)
        prev.isEnabled = startLine > 1
        next.isEnabled = window.hasMore
        val last = startLine + PAGE - 1
        status.text = "  lines $startLine–$last" + if (window.hasMore) "" else " (end)"
    }

    /** Read one window, opening (and closing) a fresh stream — the reader stops
     *  at the window edge, so the whole file is never materialized. */
    private fun readWindow(start: Int, count: Int): JsonlWindowReader.Window =
        file.inputStream.use { ins ->
            JsonlWindowReader.read(ins.bufferedReader(file.charset).lineSequence(), start, count)
        }

    override fun getComponent(): JComponent = panel
    override fun getPreferredFocusedComponent(): JComponent = table
    override fun getName(): String = "Structured"
    override fun setState(state: FileEditorState) {}
    override fun isModified(): Boolean = false
    override fun isValid(): Boolean = file.isValid
    override fun addPropertyChangeListener(listener: PropertyChangeListener) {}
    override fun removePropertyChangeListener(listener: PropertyChangeListener) {}
    override fun dispose() {}
    override fun getFile(): VirtualFile = file

    private class WindowTableModel : AbstractTableModel() {
        private var columns: List<String> = emptyList()
        private var rows: List<JsonlRow> = emptyList()

        fun update(columns: List<String>, rows: List<JsonlRow>) {
            this.columns = columns
            this.rows = rows
            fireTableStructureChanged()
        }

        override fun getRowCount() = rows.size
        override fun getColumnCount() = columns.size + 1
        override fun getColumnName(c: Int) = if (c == 0) "#" else columns[c - 1]

        override fun getValueAt(r: Int, c: Int): Any {
            val row = rows[r]
            if (c == 0) return row.lineNumber
            return when (row) {
                is JsonlRow.Record -> JsonlEngine.cell(row, columns[c - 1])
                is JsonlRow.Raw -> if (c == 1) row.text else ""
            }
        }
    }

    companion object {
        /** Physical lines per window — bounds memory on huge files (§8.2.2). */
        private const val PAGE = 1000
    }
}
