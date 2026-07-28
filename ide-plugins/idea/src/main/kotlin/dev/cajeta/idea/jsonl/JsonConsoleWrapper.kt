package dev.cajeta.idea.jsonl

import com.intellij.execution.process.ProcessEvent
import com.intellij.execution.process.ProcessHandler
import com.intellij.execution.process.ProcessListener
import com.intellij.execution.ui.ConsoleView
import com.intellij.execution.ui.ConsoleViewContentType
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.fileEditor.OpenFileDescriptor
import com.intellij.openapi.project.Project
import com.intellij.openapi.util.Key
import com.intellij.openapi.vfs.LocalFileSystem
import com.intellij.ui.JBColor
import com.intellij.ui.components.JBScrollPane
import com.intellij.ui.table.JBTable
import java.awt.BorderLayout
import java.awt.CardLayout
import java.awt.Component
import java.awt.Cursor
import java.awt.event.MouseAdapter
import java.awt.event.MouseEvent
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean
import javax.swing.JComboBox
import javax.swing.JComponent
import javax.swing.JPanel
import javax.swing.JTable
import javax.swing.JTextField
import javax.swing.JToggleButton
import javax.swing.JToolBar
import javax.swing.table.DefaultTableCellRenderer

/**
 * The in-place JSON view on a session console (json-viewer spec §3.1.1, 1.3.3):
 * a ConsoleView that IS the wrapped platform console — every call delegates —
 * whose component is a card panel flipping between the untouched platform
 * console (raw card) and a structured JSONL table, both fed from the same
 * stream. Platform console behaviors (links, search, wrap) are never
 * reimplemented; the raw card is the real console component.
 *
 * Feeding taps exactly once per line of output:
 * - live process output arrives via our [attachToProcess] listener (the
 *   delegate renders it through its own internal listener), and
 * - direct/replayed [print] calls (launch narration, pre-attach replay) go
 *   through our override.
 * Appends are batched onto the EDT (§3.1.5): any number of chunks between EDT
 * turns coalesce into one table refresh; the JBTable renders only visible rows.
 */
class JsonConsoleWrapper(
    private val delegate: ConsoleView,
    structuredByDefault: Boolean = true,
    private val project: Project? = null,
    private val navigationRoots: List<String> = emptyList(),
) : ConsoleView by delegate {

    private val controller = JsonlConsoleController(structuredByDefault)

    private val tableModel = JsonlRowsTableModel()
    private val table = JBTable(tableModel).apply {
        setDefaultRenderer(Any::class.java, TintRenderer(tableModel))
        autoResizeMode = JTable.AUTO_RESIZE_LAST_COLUMN
        installRowNavigation(this)
    }

    private val cards = CardLayout()
    private val cardPanel = JPanel(cards)
    private var panel: JPanel? = null
    private val refreshQueued = AtomicBoolean(false)

    override fun getComponent(): JComponent {
        panel?.let { return it }
        cardPanel.add(delegate.component, RAW)
        cardPanel.add(JBScrollPane(table), STRUCTURED)
        val p = JPanel(BorderLayout())
        p.add(buildToolbar(), BorderLayout.NORTH)
        p.add(cardPanel, BorderLayout.CENTER)
        panel = p
        showCard()
        return p
    }

    override fun attachToProcess(processHandler: ProcessHandler) {
        delegate.attachToProcess(processHandler)
        processHandler.addProcessListener(object : ProcessListener {
            override fun onTextAvailable(event: ProcessEvent, outputType: Key<*>) {
                appendChunk(event.text)
            }
        })
    }

    override fun print(text: String, contentType: ConsoleViewContentType) {
        delegate.print(text, contentType)
        appendChunk(text)
    }

    override fun clear() {
        delegate.clear()
        synchronized(controller) { controller.clear() }
        scheduleRefresh()
    }

    /** Any thread; one EDT refresh per burst of chunks. */
    private fun appendChunk(text: String) {
        synchronized(controller) { controller.append(text) }
        scheduleRefresh()
    }

    private fun scheduleRefresh() {
        if (!refreshQueued.compareAndSet(false, true)) return
        val app = ApplicationManager.getApplication()
        if (app == null || app.isDispatchThread) {
            refreshQueued.set(false)
            refresh()
        } else {
            app.invokeLater {
                refreshQueued.set(false)
                refresh()
            }
        }
    }

    private fun refresh() {
        val (columns, visible) = synchronized(controller) {
            controller.model().columns to controller.visibleRows()
        }
        tableModel.update(columns, visible)
    }

    private fun buildToolbar(): JToolBar = JToolBar().apply {
        isFloatable = false
        add(JToggleButton("JSON", controller.isStructured).apply {
            toolTipText = "Toggle structured JSON view / platform console"
            addActionListener {
                synchronized(controller) { controller.setStructured(isSelected) }
                showCard()
                if (isSelected) refresh()
            }
        })
        add(levelFilter.apply {
            addActionListener {
                val sel = selectedItem as String
                synchronized(controller) {
                    if (sel == "all") controller.clearFilter() else controller.setLevelFilter(sel)
                }
                refresh()
            }
        })
        add(fieldFilter)
        add(JToggleButton("Filter field").apply {
            addActionListener { applyFieldFilter(); refresh() }
        })
    }

    private val levelFilter =
        JComboBox(arrayOf("all", "trace", "debug", "info", "warn", "error", "fatal"))
    private val fieldFilter = JTextField(12)

    /** Field filter as `key=value`; blank clears it (level filter takes over). */
    private fun applyFieldFilter() {
        val raw = fieldFilter.text.trim()
        val eq = raw.indexOf('=')
        synchronized(controller) {
            if (eq > 0) controller.setFieldFilter(raw.substring(0, eq), raw.substring(eq + 1))
            else if (raw.isEmpty() && (levelFilter.selectedItem as String) == "all") controller.clearFilter()
        }
    }

    private fun showCard() =
        cards.show(cardPanel, if (controller.isStructured) STRUCTURED else RAW)

    // --- diagnostic row navigation (§3.1.6): a row with resolvable source
    //     coordinates is clickable and jumps to the location; hovering one
    //     shows a hand cursor. Extraction/resolution are the pure
    //     JsonlRowNavigation; only the jump itself touches the platform. ---

    private fun installRowNavigation(table: JBTable) {
        table.addMouseListener(object : MouseAdapter() {
            override fun mouseClicked(e: MouseEvent) {
                if (e.clickCount != 1) return
                resolvedLocationAt(table.rowAtPoint(e.point))?.let { (path, loc) -> navigateTo(path, loc) }
            }
        })
        table.addMouseMotionListener(object : MouseAdapter() {
            override fun mouseMoved(e: MouseEvent) {
                val navigable = resolvedLocationAt(table.rowAtPoint(e.point)) != null
                table.cursor =
                    if (navigable) Cursor.getPredefinedCursor(Cursor.HAND_CURSOR)
                    else Cursor.getDefaultCursor()
            }
        })
    }

    private fun resolvedLocationAt(viewRow: Int): Pair<String, RowLocation>? {
        if (viewRow < 0 || project == null) return null
        val row = tableModel.rowAt(table.convertRowIndexToModel(viewRow)) ?: return null
        val loc = JsonlRowNavigation.locationOf(row) ?: return null
        val roots = navigationRoots.ifEmpty { listOfNotNull(project.basePath) }
        val path = JsonlRowNavigation.resolve(loc, roots) { File(it).isFile } ?: return null
        return path to loc
    }

    private fun navigateTo(path: String, loc: RowLocation) {
        val proj = project ?: return
        val vf = LocalFileSystem.getInstance().refreshAndFindFileByIoFile(File(path)) ?: return
        OpenFileDescriptor(proj, vf, loc.line - 1, loc.column - 1).navigate(true)
    }

    /** Level-based row coloring (§3.1.3) over the shared tint mapping. */
    private class TintRenderer(private val model: JsonlRowsTableModel) : DefaultTableCellRenderer() {
        override fun getTableCellRendererComponent(
            table: JTable, value: Any?, isSelected: Boolean, hasFocus: Boolean, row: Int, column: Int,
        ): Component {
            val c = super.getTableCellRendererComponent(table, value, isSelected, hasFocus, row, column)
            if (!isSelected) {
                val modelRow = table.convertRowIndexToModel(row)
                c.foreground = when (model.rowAt(modelRow)?.let { JsonlEngine.tintOf(it) }) {
                    RowTint.ERROR -> JBColor.RED
                    RowTint.WARN -> JBColor.ORANGE
                    else -> table.foreground
                }
            }
            return c
        }
    }

    companion object {
        private const val STRUCTURED = "structured"
        private const val RAW = "raw"
    }
}
