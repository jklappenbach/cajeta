package dev.cajeta.idea.jsonl

import com.intellij.execution.process.ProcessEvent
import com.intellij.execution.process.ProcessHandler
import com.intellij.execution.process.ProcessListener
import com.intellij.execution.ui.ConsoleView
import com.intellij.execution.ui.ConsoleViewContentType
import com.intellij.icons.AllIcons
import com.intellij.openapi.actionSystem.ActionGroup
import com.intellij.openapi.actionSystem.ActionManager
import com.intellij.openapi.actionSystem.ActionUpdateThread
import com.intellij.openapi.actionSystem.AnAction
import com.intellij.openapi.actionSystem.AnActionEvent
import com.intellij.openapi.actionSystem.DefaultActionGroup
import com.intellij.openapi.actionSystem.Separator
import com.intellij.openapi.actionSystem.ToggleAction
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.fileEditor.OpenFileDescriptor
import com.intellij.openapi.project.DumbAware
import com.intellij.openapi.project.Project
import com.intellij.openapi.util.Key
import com.intellij.openapi.vfs.LocalFileSystem
import com.intellij.ui.JBColor
import com.intellij.ui.SearchTextField
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
import javax.swing.JComponent
import javax.swing.JPanel
import javax.swing.JTable
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
    /** Run/debug profile this console belongs to (§3.1.9.1). Blank disables
     *  layout persistence, which is what a console with no owning
     *  configuration wants. Build with [JsonConsoleLayoutStore.keyFor]. */
    private val profileKey: String = "",
) : ConsoleView by delegate {

    private val controller = JsonlConsoleController(structuredByDefault)

    private val tableModel = JsonlRowsTableModel()
    private val table = JBTable(tableModel).apply {
        setDefaultRenderer(Any::class.java, TintRenderer(tableModel))
        // No width ceiling (§3.1.8): columns size to content and the scroll pane
        // scrolls horizontally, instead of every column being squeezed into the
        // viewport and a long message ending in an ellipsis.
        autoResizeMode = JTable.AUTO_RESIZE_OFF
        installRowNavigation(this)
        installLayoutCapture(this)
    }

    private val cards = CardLayout()
    private val cardPanel = JPanel(cards)
    private var panel: JPanel? = null
    private val refreshQueued = AtomicBoolean(false)

    private val layoutStore: JsonConsoleLayoutStore? =
        if (profileKey.isBlank() || project == null) null
        else JsonConsoleLayoutStore.getInstance(project)

    init {
        // Restore this profile's layout before any output arrives, so the
        // first refresh already renders the reader's arrangement (§3.1.9).
        layoutStore?.load(profileKey)?.let { controller.columns.applyLayout(it) }
    }

    /** Persist the live arrangement. Called on every explicit change — a
     *  field toggle, a header drag, a resize — because any of them may be the
     *  last thing that happens before the session ends. */
    private fun saveLayout() {
        val store = layoutStore ?: return
        val layout = synchronized(controller) { controller.columns.currentLayout() }
        store.save(profileKey, layout)
    }

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
            controller.visibleColumns() to controller.visibleRows()
        }
        tableModel.update(columns, visible)
        synchronized(controller) { JsonlTableSupport.applyWidths(table, columns, controller.columns) }
    }

    // --- toolbar: platform ActionToolbar so the flip reads like any other
    //     console control (plan 2.2.4) — an icon ToggleAction for JSON/text, a
    //     funnel popup for the level floor, a SearchTextField for `key=value`. ---

    private var minLevel: String = "all"

    private fun buildToolbar(): JComponent {
        val group = DefaultActionGroup(
            JsonToggleAction(), Separator.getInstance(), FieldsGroup(), LevelFilterGroup())
        val toolbar = ActionManager.getInstance()
            .createActionToolbar("CajetaJsonConsole", group, true)
        toolbar.targetComponent = cardPanel
        return JPanel(BorderLayout()).apply {
            add(toolbar.component, BorderLayout.WEST)
            add(fieldSearch, BorderLayout.EAST)
        }
    }

    private inner class JsonToggleAction : ToggleAction(
        "Structured JSON View",
        "Flip between the structured JSON table and the platform console",
        AllIcons.FileTypes.Json,
    ), DumbAware {
        override fun getActionUpdateThread(): ActionUpdateThread = ActionUpdateThread.EDT
        override fun isSelected(e: AnActionEvent): Boolean = controller.isStructured
        override fun setSelected(e: AnActionEvent, state: Boolean) {
            synchronized(controller) { controller.setStructured(state) }
            showCard()
            if (state) refresh()
        }
    }

    /** The column chooser (§3.1.7): children are computed at popup time from
     *  the live discovery list, so a field the run only revealed a moment ago
     *  is there without a reload. */
    private inner class FieldsGroup : ActionGroup("Fields", true), DumbAware {
        init {
            templatePresentation.icon = AllIcons.Actions.ShowImportStatements
            templatePresentation.description = "Choose which JSON fields are shown as columns"
        }

        override fun getActionUpdateThread(): ActionUpdateThread = ActionUpdateThread.EDT

        override fun getChildren(e: AnActionEvent?): Array<AnAction> {
            val fields = synchronized(controller) { controller.columns.available() }
            if (fields.isEmpty()) return arrayOf(disabledItem("No fields discovered yet"))
            val items = ArrayList<AnAction>(fields.size + 2)
            for (field in fields) items += FieldToggle(field)
            items += Separator.getInstance()
            items += object : AnAction("Reset to Defaults"), DumbAware {
                override fun actionPerformed(e: AnActionEvent) {
                    synchronized(controller) { controller.columns.resetToDefaults() }
                    refresh()
                    saveLayout()
                }
            }
            return items.toTypedArray()
        }

        private fun disabledItem(text: String): AnAction = object : AnAction(text), DumbAware {
            override fun getActionUpdateThread(): ActionUpdateThread = ActionUpdateThread.EDT
            override fun update(e: AnActionEvent) { e.presentation.isEnabled = false }
            override fun actionPerformed(e: AnActionEvent) {}
        }
    }

    private inner class FieldToggle(private val field: String) : ToggleAction(field), DumbAware {
        override fun getActionUpdateThread(): ActionUpdateThread = ActionUpdateThread.EDT
        override fun isSelected(e: AnActionEvent): Boolean =
            synchronized(controller) { controller.columns.isVisible(field) }
        override fun setSelected(e: AnActionEvent, state: Boolean) {
            synchronized(controller) { controller.columns.setFieldVisible(field, state) }
            refresh()
            saveLayout()
        }
    }

    private inner class LevelFilterGroup : DefaultActionGroup(
        "Filter by Level", listOf<AnAction>(),
    ), DumbAware {
        init {
            templatePresentation.icon = AllIcons.General.Filter
            templatePresentation.description = "Show records at or above a level"
            isPopup = true
            for (level in listOf("all", "trace", "debug", "info", "warn", "error", "fatal")) {
                add(object : ToggleAction(level), DumbAware {
                    override fun getActionUpdateThread(): ActionUpdateThread = ActionUpdateThread.EDT
                    override fun isSelected(e: AnActionEvent): Boolean = minLevel == level
                    override fun setSelected(e: AnActionEvent, state: Boolean) {
                        if (!state) return
                        minLevel = level
                        applyFilters()
                    }
                })
            }
        }
        override fun getActionUpdateThread(): ActionUpdateThread = ActionUpdateThread.EDT
    }

    private val fieldSearch = SearchTextField(false).apply {
        textEditor.emptyText.text = "field=value"
        textEditor.columns = 14
        textEditor.addActionListener { applyFilters() }
    }

    /** One place combines both controls: `key=value` narrows fields; otherwise
     *  the level floor applies; both empty clears the filter. */
    private fun applyFilters() {
        val raw = fieldSearch.text.trim()
        val eq = raw.indexOf('=')
        synchronized(controller) {
            when {
                eq > 0 -> controller.setFieldFilter(raw.substring(0, eq), raw.substring(eq + 1))
                minLevel != "all" -> controller.setLevelFilter(minLevel)
                else -> controller.clearFilter()
            }
        }
        refresh()
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

    // --- layout capture (§3.1.9): the reader's own gestures, and only those.
    //     A drag on the header is what marks a width as reader-set (§3.1.9.3);
    //     the viewer's own content sizing must never pin a column, or every
    //     column would freeze at whatever the first burst happened to need. ---

    private fun installLayoutCapture(table: JBTable) {
        if (layoutStore == null) return
        val header = table.tableHeader ?: return
        header.reorderingAllowed = true
        header.addMouseListener(object : MouseAdapter() {
            override fun mouseReleased(e: MouseEvent) {
                // Read the arrangement AFTER the gesture completes: mid-drag
                // the column model is still moving.
                captureColumnOrder()
                captureResizedWidth()
                saveLayout()
            }
        })
    }

    /** The visible columns in the order the header now shows them. */
    private fun captureColumnOrder() {
        val model = table.columnModel
        val names = ArrayList<String>(model.columnCount)
        for (i in 0 until model.columnCount) {
            // Column 0 is the `#` line column, which is not a field.
            val name = model.getColumn(i).headerValue?.toString() ?: continue
            if (name != "#" && name != "line") names += name
        }
        if (names.isNotEmpty()) synchronized(controller) { controller.columns.setOrder(names) }
    }

    /** The width of whichever column the reader was dragging, if any. */
    private fun captureResizedWidth() {
        val resized = table.tableHeader?.resizingColumn ?: return
        val name = resized.headerValue?.toString() ?: return
        if (name == "#" || name == "line") return
        synchronized(controller) { controller.columns.setUserWidth(name, resized.width) }
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
            val modelRow = table.convertRowIndexToModel(row)
            // A raw passthrough line doesn't widen its column (§3.1.8.1), so
            // hovering is how a long one is read without flipping to raw.
            toolTipText = if (model.rowAt(modelRow) is JsonlRow.Raw && column == 1)
                model.lineTextAt(modelRow) else null
            if (!isSelected) {
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
