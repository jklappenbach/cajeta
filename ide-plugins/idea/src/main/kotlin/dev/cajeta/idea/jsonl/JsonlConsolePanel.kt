package dev.cajeta.idea.jsonl

import com.intellij.execution.process.ProcessEvent
import com.intellij.execution.process.ProcessHandler
import com.intellij.execution.process.ProcessListener
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.ui.SimpleToolWindowPanel
import com.intellij.ui.components.JBScrollPane
import com.intellij.ui.components.JBTextArea
import com.intellij.ui.table.JBTable
import java.awt.BorderLayout
import java.awt.CardLayout
import javax.swing.JComboBox
import javax.swing.JPanel
import javax.swing.JTextField
import javax.swing.JToggleButton
import javax.swing.JToolBar
import javax.swing.table.AbstractTableModel

/**
 * The structured JSONL console surface (spec §7): a thin Swing delegate over the
 * pure [JsonlConsoleController]. A toolbar toggles structured/raw and applies a
 * level / field-substring filter; the structured view is a table (columns from
 * the shared §8 engine, raw passthrough rows shown verbatim), the raw view is the
 * untouched stream. Feed it with [append] (run stdout) or [loadText]
 * (a `.cajeta/logs` JSONL file). All mutation marshals onto the EDT. Behavior lives in
 * the controller; this class only renders — exercised via `runIde`.
 */
class JsonlConsolePanel(structuredByDefault: Boolean = true) : SimpleToolWindowPanel(true, true) {

    private val controller = JsonlConsoleController(structuredByDefault)

    private val tableModel = RowsTableModel()
    private val table = JBTable(tableModel)
    private val rawArea = JBTextArea().apply { isEditable = false }

    private val cards = CardLayout()
    private val cardPanel = JPanel(cards)
    private val levelFilter = JComboBox(arrayOf("all", "trace", "debug", "info", "warn", "error", "fatal"))
    private val fieldFilter = JTextField(12)

    init {
        cardPanel.add(JBScrollPane(table), STRUCTURED)
        cardPanel.add(JBScrollPane(rawArea), RAW)
        setContent(cardPanel)
        setToolbar(buildToolbar(structuredByDefault))
        showCard()
    }

    private fun buildToolbar(structuredByDefault: Boolean): JToolBar = JToolBar().apply {
        isFloatable = false
        add(JToggleButton("Structured", structuredByDefault).apply {
            addActionListener {
                controller.setStructured(isSelected)
                text = if (isSelected) "Structured" else "Raw"
                refresh()
            }
        })
        add(levelFilter.apply {
            addActionListener {
                val sel = selectedItem as String
                if (sel == "all") controller.clearFilter() else controller.setLevelFilter(sel)
                if (fieldFilter.text.isNotBlank() && sel == "all") applyFieldFilter()
                refresh()
            }
        })
        add(fieldFilter)
        add(JToggleButton("Filter field").apply {
            addActionListener { applyFieldFilter(); refresh() }
        })
    }

    /** Field filter as `key=value`; blank clears it (level filter takes over). */
    private fun applyFieldFilter() {
        val raw = fieldFilter.text.trim()
        val eq = raw.indexOf('=')
        if (eq > 0) controller.setFieldFilter(raw.substring(0, eq), raw.substring(eq + 1))
        else if (raw.isEmpty() && (levelFilter.selectedItem as String) == "all") controller.clearFilter()
    }

    /** Append a chunk of run output (any thread). */
    fun append(text: String) = onEdt {
        controller.append(text)
        refresh()
    }

    /** Replace the console with a log file's full contents (any thread). */
    fun loadText(text: String) = onEdt {
        controller.append(text)
        refresh()
    }

    /** Subscribe to a run's stdout/stderr so the structured view fills live as
     *  the process emits JSONL (spec §7.2.1). The platform delivers text on its
     *  own thread; [append] marshals to the EDT. */
    fun attachTo(processHandler: ProcessHandler) {
        processHandler.addProcessListener(object : ProcessListener {
            override fun onTextAvailable(event: ProcessEvent, outputType: com.intellij.openapi.util.Key<*>) {
                append(event.text)
            }
        })
    }

    private fun refresh() {
        showCard()
        if (controller.isStructured) {
            tableModel.update(controller.model().columns, controller.visibleRows())
        } else {
            rawArea.text = controller.rawText()
        }
    }

    private fun showCard() = cards.show(cardPanel, if (controller.isStructured) STRUCTURED else RAW)

    private fun onEdt(block: () -> Unit) {
        val app = ApplicationManager.getApplication()
        if (app == null || app.isDispatchThread) block() else app.invokeLater(block)
    }

    /** Table model over the engine's columns: one leading `#` line column, then a
     *  cell per derived column; a [JsonlRow.Raw] spans as its verbatim text in
     *  the first data column. */
    private class RowsTableModel : AbstractTableModel() {
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
                is JsonlRow.Raw -> if (c == 1) row.text else ""   // raw text in first data column
            }
        }
    }

    companion object {
        private const val STRUCTURED = "structured"
        private const val RAW = "raw"
    }
}
