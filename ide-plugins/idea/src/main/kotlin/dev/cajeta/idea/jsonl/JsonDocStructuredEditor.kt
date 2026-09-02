package dev.cajeta.idea.jsonl

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.command.WriteCommandAction
import com.intellij.openapi.editor.Document
import com.intellij.openapi.editor.event.DocumentEvent
import com.intellij.openapi.editor.event.DocumentListener
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.openapi.fileEditor.FileEditor
import com.intellij.openapi.fileEditor.FileEditorState
import com.intellij.openapi.project.Project
import com.intellij.openapi.util.UserDataHolderBase
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.ui.components.JBLabel
import com.intellij.ui.components.JBScrollPane
import com.intellij.ui.table.JBTable
import com.intellij.ui.treeStructure.treetable.ListTreeTableModelOnColumns
import com.intellij.ui.treeStructure.treetable.TreeTable
import com.intellij.ui.treeStructure.treetable.TreeTableModel
import com.intellij.util.ui.ColumnInfo
import dev.cajeta.idea.debugger.Json
import java.awt.BorderLayout
import java.awt.CardLayout
import java.beans.PropertyChangeListener
import java.util.concurrent.atomic.AtomicBoolean
import javax.swing.JComponent
import javax.swing.JPanel
import javax.swing.JTextField
import javax.swing.JToolBar
import javax.swing.tree.DefaultMutableTreeNode
import javax.swing.tree.TreePath

/**
 * The structured tab for a JSON document (json-viewer spec §4): a Key/Value
 * tree over [JsonDocModel] with editable scalar cells. The platform Document is
 * the single source (§4.1.3): an edited cell becomes one [JsonDocEdit.TextEdit]
 * applied in one write command (one undo step); the tree rebuilds from the
 * Document-change notification, which also keeps it live while the text tab is
 * edited. Malformed text shows the located error, never a dead tab (§2.2.4);
 * documents above the size threshold are read-only with a banner (§4.1.5). A
 * top-level array of flat objects renders as a table with the engine's column
 * inference (§4.1.2).
 */
class JsonDocStructuredEditor(
    private val project: Project,
    private val file: VirtualFile,
) : UserDataHolderBase(), FileEditor {

    private val document: Document? = FileDocumentManager.getInstance().getDocument(file)

    private val cards = CardLayout()
    private val cardPanel = JPanel(cards)
    private val panel = JPanel(BorderLayout())
    private val breadcrumb = JBLabel(" ")
    private val find = JTextField(16)
    private val errorLabel = JBLabel()
    private val banner = JBLabel()

    private val flatTableModel = JsonlRowsTableModel()
    private var treeTable: TreeTable? = null
    private var root: JsonDocNode? = null
    private val rebuildQueued = AtomicBoolean(false)

    private val documentListener = object : DocumentListener {
        override fun documentChanged(event: DocumentEvent) = scheduleRebuild()
    }

    init {
        val bar = JToolBar().apply {
            isFloatable = false
            add(JBLabel(" Find: "))
            add(find.apply { addActionListener { findNext(text) } })
            add(breadcrumb)
        }
        panel.add(bar, BorderLayout.NORTH)
        panel.add(banner, BorderLayout.SOUTH)
        banner.isVisible = false
        cardPanel.add(JBScrollPane(errorLabel), ERROR)
        cardPanel.add(JBScrollPane(JsonlRowsTable(flatTableModel)), TABLE)
        panel.add(cardPanel, BorderLayout.CENTER)
        document?.addDocumentListener(documentListener)
        rebuild()
    }

    private val editable: Boolean
        get() = document != null && document.isWritable &&
            JsonDocEdit.isEditableSize(document.textLength.toLong())

    private fun scheduleRebuild() {
        if (!rebuildQueued.compareAndSet(false, true)) return
        ApplicationManager.getApplication().invokeLater {
            rebuildQueued.set(false)
            rebuild()
        }
    }

    private fun rebuild() {
        val text = document?.text ?: return showError("no document for ${file.name}")
        when (val parsed = JsonDocModel.parse(text, lenient = true)) {
            is JsonDocResult.Err ->
                showError("parse error at ${parsed.line}:${parsed.column} — ${parsed.message}")
            is JsonDocResult.Ok -> {
                root = parsed.root
                banner.isVisible = !editable
                if (!editable && document != null &&
                    !JsonDocEdit.isEditableSize(document.textLength.toLong())
                ) {
                    banner.text = " Read-only: document exceeds the structured-editing size threshold"
                }
                val flat = parsed.root.asFlatObjectArray()
                if (flat != null) showFlatTable(flat) else showTree(parsed.root)
            }
        }
    }

    private fun showError(message: String) {
        errorLabel.text = " $message"
        cards.show(cardPanel, ERROR)
    }

    // --- flat-object-array table mode (§4.1.2) ---

    private fun JsonDocNode.asFlatObjectArray(): List<JsonlRow>? {
        val arr = this as? JsonDocNode.ArrayNode ?: return null
        if (arr.elements.isEmpty()) return null
        val rows = arr.elements.mapIndexed { i, e ->
            val obj = e as? JsonDocNode.ObjectNode ?: return null
            if (obj.entries.any { it.value !is JsonDocNode.Scalar }) return null
            JsonlRow.Record(i + 1, (obj.toJson() as Json.Obj).entries, "")
        }
        return rows
    }

    private fun showFlatTable(rows: List<JsonlRow>) {
        flatTableModel.update(JsonlEngine.columnsOf(rows), rows)
        cards.show(cardPanel, TABLE)
    }

    // --- tree mode ---

    private fun showTree(rootNode: JsonDocNode) {
        val treeRoot = buildTreeNode(file.nameWithoutExtension, rootNode)
        val model = ListTreeTableModelOnColumns(treeRoot, arrayOf(KeyColumn(), ValueColumn()))
        val tt = TreeTable(model)
        tt.tree.isRootVisible = true
        tt.tree.addTreeSelectionListener { breadcrumb.text = "  " + breadcrumbOf(it.path) }
        treeTable = tt
        // replace (or add) the tree card with the fresh instance
        cardPanel.add(JBScrollPane(tt), TREE)
        cards.show(cardPanel, TREE)
    }

    private class Row(val label: String, val node: JsonDocNode)

    private fun buildTreeNode(label: String, node: JsonDocNode): DefaultMutableTreeNode {
        val treeNode = DefaultMutableTreeNode(Row(label, node))
        when (node) {
            is JsonDocNode.ObjectNode ->
                node.entries.forEach { treeNode.add(buildTreeNode(it.key, it.value)) }
            is JsonDocNode.ArrayNode ->
                node.elements.forEachIndexed { i, e -> treeNode.add(buildTreeNode("[$i]", e)) }
            is JsonDocNode.Scalar -> {}
        }
        return treeNode
    }

    private fun breadcrumbOf(path: TreePath): String =
        path.path.joinToString(".") {
            ((it as DefaultMutableTreeNode).userObject as Row).label
        }

    private fun findNext(query: String) {
        val tt = treeTable ?: return
        if (query.isBlank()) return
        val rootNode = tt.tree.model.root as DefaultMutableTreeNode
        val all = rootNode.depthFirstEnumeration().toList().filterIsInstance<DefaultMutableTreeNode>()
        val selected = tt.tree.selectionPath?.lastPathComponent
        val startAfter = all.indexOfFirst { it === selected }
        val ordered = all.drop(startAfter + 1) + all.take(startAfter + 1)
        val q = query.lowercase()
        val hit = ordered.firstOrNull { n ->
            val row = n.userObject as Row
            row.label.lowercase().contains(q) ||
                (row.node as? JsonDocNode.Scalar)?.value?.toCompactString()?.lowercase()?.contains(q) == true
        } ?: return
        val path = TreePath(hit.path)
        tt.tree.expandPath(path.parentPath)
        tt.tree.selectionPath = path
        tt.tree.scrollPathToVisible(path)
    }

    private inner class KeyColumn : ColumnInfo<Any, Any>("Key") {
        override fun valueOf(item: Any): Any = item
        override fun getColumnClass(): Class<*> = TreeTableModel::class.java
    }

    private inner class ValueColumn : ColumnInfo<Any, String>("Value") {
        override fun valueOf(item: Any): String {
            val row = (item as? DefaultMutableTreeNode)?.userObject as? Row ?: return ""
            return when (val n = row.node) {
                is JsonDocNode.Scalar -> n.value.toCompactString()
                is JsonDocNode.ObjectNode -> "{${n.entries.size}}"
                is JsonDocNode.ArrayNode -> "[${n.elements.size}]"
            }
        }

        override fun isCellEditable(item: Any): Boolean =
            editable && ((item as? DefaultMutableTreeNode)?.userObject as? Row)?.node is JsonDocNode.Scalar

        override fun setValue(item: Any, value: String) {
            val row = (item as? DefaultMutableTreeNode)?.userObject as? Row ?: return
            val scalar = row.node as? JsonDocNode.Scalar ?: return
            val doc = document ?: return
            val newValue = parseScalarInput(value) ?: return
            if (newValue == scalar.value) return
            val edit = JsonDocEdit.setScalar(scalar, newValue)
            // one edit = one undoable write command (§4.1.3); the document
            // listener rebuilds the tree from the changed text.
            WriteCommandAction.runWriteCommandAction(project) {
                doc.replaceString(edit.start, edit.end, edit.replacement)
            }
        }
    }

    /** A cell's text as a JSON scalar: valid scalar JSON stands (numbers, true,
     *  null, quoted strings); anything else is taken as a plain string. */
    private fun parseScalarInput(text: String): Json? {
        val t = text.trim()
        if (t.isEmpty()) return Json.Str("")
        val parsed = try {
            Json.parse(t)
        } catch (_: Exception) {
            null
        }
        return when (parsed) {
            is Json.Obj, is Json.Arr -> null   // scalar cells hold scalars
            null -> Json.Str(text)
            else -> parsed
        }
    }

    override fun getComponent(): JComponent = panel
    override fun getPreferredFocusedComponent(): JComponent = treeTable ?: panel
    override fun getName(): String = "Structured"
    override fun setState(state: FileEditorState) {}
    override fun isModified(): Boolean = false
    override fun isValid(): Boolean = file.isValid
    override fun addPropertyChangeListener(listener: PropertyChangeListener) {}
    override fun removePropertyChangeListener(listener: PropertyChangeListener) {}
    override fun dispose() {
        document?.removeDocumentListener(documentListener)
    }
    override fun getFile(): VirtualFile = file

    companion object {
        private const val TREE = "tree"
        private const val TABLE = "table"
        private const val ERROR = "error"
    }
}
