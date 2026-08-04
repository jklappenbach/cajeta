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
import javax.swing.JTable
import javax.swing.JToolBar
import java.awt.BorderLayout

/**
 * The structured tab for a `.jsonl` file (spec §8): a paged table over
 * [JsonlWindowReader], so even a multi-hundred-MB file opens responsively — only
 * one [PAGE] window is read at a time (§8.2.2). Cells render through the shared
 * §8 [JsonlEngine], matching the console (§8.2.3). Read-only.
 */
class JsonlStructuredEditor(private val file: VirtualFile) : UserDataHolderBase(), FileEditor {

    private val tableModel = JsonlRowsTableModel()
    // No width ceiling (§3.1.8): content-sized columns, horizontal scrolling.
    private val table = JBTable(tableModel).apply { autoResizeMode = JTable.AUTO_RESIZE_OFF }
    private val status = JLabel()
    private val prev = JButton("◀ Prev")
    private val next = JButton("Next ▶")
    private val fields = JButton("Fields")
    private val panel = JPanel(BorderLayout())

    // Column discovery/selection/widths (§4.1.6), owned across pages: paging
    // forward may reveal fields the first window never had, and paging back
    // must not retract them or discard the reader's choice.
    private val columns = JsonlColumns()

    private var startLine = 1

    init {
        val bar = JToolBar().apply {
            isFloatable = false
            add(prev.apply { addActionListener { page(-PAGE) } })
            add(next.apply { addActionListener { page(PAGE) } })
            add(fields.apply {
                addActionListener {
                    JsonlTableSupport.fieldsPopup(columns) { render() }.show(this, 0, height)
                }
            })
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

    private var rows: List<JsonlRow> = emptyList()

    private fun load() {
        val window = readWindow(startLine, PAGE)
        rows = window.rows
        columns.observeAll(window.rows)
        render()
        prev.isEnabled = startLine > 1
        next.isEnabled = window.hasMore
        val last = startLine + PAGE - 1
        status.text = "  lines $startLine–$last" + if (window.hasMore) "" else " (end)"
    }

    /** Re-render the loaded window under the current column selection. */
    private fun render() {
        val visible = columns.visible()
        tableModel.update(visible, rows)
        JsonlTableSupport.applyWidths(table, visible, columns)
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

    companion object {
        /** Physical lines per window — bounds memory on huge files (§8.2.2). */
        private const val PAGE = 1000
    }
}
