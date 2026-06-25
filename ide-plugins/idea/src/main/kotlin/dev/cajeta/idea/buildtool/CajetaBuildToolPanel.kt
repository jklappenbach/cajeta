package dev.cajeta.idea.buildtool

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.fileEditor.OpenFileDescriptor
import com.intellij.openapi.progress.ProgressIndicator
import com.intellij.openapi.progress.Task
import com.intellij.openapi.project.Project
import com.intellij.openapi.ui.SimpleToolWindowPanel
import com.intellij.openapi.vfs.LocalFileSystem
import com.intellij.ui.ColoredTreeCellRenderer
import com.intellij.ui.SimpleTextAttributes
import com.intellij.ui.TreeSpeedSearch
import com.intellij.ui.components.JBScrollPane
import com.intellij.ui.treeStructure.Tree
import com.intellij.util.ui.tree.TreeUtil
import dev.cajeta.idea.settings.CajetaSettings
import java.awt.event.KeyEvent
import java.awt.event.MouseAdapter
import java.awt.event.MouseEvent
import java.nio.charset.StandardCharsets
import javax.swing.JComponent
import javax.swing.JMenuItem
import javax.swing.JPopupMenu
import javax.swing.JTree
import javax.swing.KeyStroke
import javax.swing.SwingUtilities
import javax.swing.tree.DefaultMutableTreeNode
import javax.swing.tree.DefaultTreeModel

/**
 * The Cajeta build-tool tool window content: a grouped task tree backed by
 * `cajeta tasks --json` discovery (spec §2, §4). Discovery runs off the EDT;
 * double-click / Enter / context-menu Run launch the task (unit 5 minimal
 * launcher; unit 6 upgrades to a streaming console). The tree model + node
 * grouping live in plain-JVM cores ([TaskTreeModelBuilder], [ManifestTaskLocator]).
 */
class CajetaBuildToolPanel(private val project: Project) : SimpleToolWindowPanel(true, true) {

    /** Leaf whose toString is the label, so the default tree speed-search and
     *  renderer both read cleanly. */
    private class TaskLeaf(val data: TaskTreeNode) : DefaultMutableTreeNode(data) {
        override fun toString(): String = data.label
    }

    /** The last successfully discovered model, kept so the context menu can
     *  resolve a task's debug-launch coordinates (§5.2.2). */
    private var lastModel: TaskModel? = null

    private val rootNode = DefaultMutableTreeNode("root")
    private val treeModel = DefaultTreeModel(rootNode)
    private val tree = Tree(treeModel).apply {
        isRootVisible = false
        showsRootHandles = true
        cellRenderer = CajetaNodeRenderer()
    }

    init {
        @Suppress("DEPRECATION")
        TreeSpeedSearch(tree)
        tree.addMouseListener(object : MouseAdapter() {
            override fun mousePressed(e: MouseEvent) {
                if (SwingUtilities.isRightMouseButton(e)) {
                    val row = tree.getClosestRowForLocation(e.x, e.y)
                    if (row >= 0) tree.setSelectionRow(row)
                }
            }

            override fun mouseClicked(e: MouseEvent) {
                if (e.clickCount == 2 && SwingUtilities.isLeftMouseButton(e)) {
                    runSelected()
                } else if (SwingUtilities.isRightMouseButton(e)) {
                    showContextMenu(e)
                }
            }
        })
        tree.registerKeyboardAction(
            { runSelected() },
            KeyStroke.getKeyStroke(KeyEvent.VK_ENTER, 0),
            JComponent.WHEN_FOCUSED,
        )
        setContent(JBScrollPane(tree))
        reload()
    }

    /** Re-run discovery and rebuild the tree. */
    fun reload() {
        val manifest = CajetaManifest.path(project)
        if (manifest == null) {
            showMessage("No cajeta.json in this project")
            return
        }
        object : Task.Backgroundable(project, "Discovering Cajeta tasks", true) {
            override fun run(indicator: ProgressIndicator) {
                val result = CajetaBuildRunner.discover(CajetaSettings.instance.buildToolPath, manifest)
                ApplicationManager.getApplication().invokeLater {
                    when (result) {
                        is CajetaBuildRunner.DiscoverResult.Ok -> populate(result.model)
                        is CajetaBuildRunner.DiscoverResult.Failed -> showMessage(result.reason)
                    }
                }
            }
        }.queue()
    }

    private fun populate(model: TaskModel) {
        lastModel = model
        rootNode.removeAllChildren()
        for (group in TaskTreeModelBuilder.build(model)) {
            val groupNode = DefaultMutableTreeNode(group.title)
            for (node in group.nodes) groupNode.add(TaskLeaf(node))
            rootNode.add(groupNode)
        }
        treeModel.reload()
        TreeUtil.expandAll(tree)
    }

    private fun showMessage(text: String) {
        rootNode.removeAllChildren()
        rootNode.add(DefaultMutableTreeNode(text))
        treeModel.reload()
    }

    private fun selectedTask(): TaskTreeNode? =
        (tree.selectionPath?.lastPathComponent as? TaskLeaf)?.data

    private fun runSelected() {
        val node = selectedTask() ?: return
        val manifest = CajetaManifest.path(project) ?: return
        CajetaTaskLauncher.launch(project, manifest, node)
    }

    private fun debugSelected(node: TaskTreeNode) {
        val manifest = CajetaManifest.path(project) ?: return
        val model = lastModel ?: return
        CajetaTaskLauncher.debug(project, manifest, model, node)
    }

    /** Whether the selected node maps to a dap-debuggable task (§5.2.2). */
    private fun isDebuggable(node: TaskTreeNode): Boolean {
        if (node.kind != TaskTreeNode.Kind.TASK) return false
        val model = lastModel ?: return false
        val task = model.tasks.firstOrNull { it.name == node.runName } ?: return false
        return TaskDebugMapping.isDebuggable(task, model)
    }

    private fun openInManifest(node: TaskTreeNode) {
        val path = CajetaManifest.path(project) ?: return
        val vf = LocalFileSystem.getInstance().findFileByPath(path) ?: return
        val text = runCatching {
            String(vf.contentsToByteArray(), StandardCharsets.UTF_8)
        }.getOrDefault("")
        val offset = ManifestTaskLocator.offsetOf(text, node.runName) ?: 0
        OpenFileDescriptor(project, vf, offset).navigate(true)
    }

    private fun showContextMenu(e: MouseEvent) {
        val node = selectedTask() ?: return
        JPopupMenu().apply {
            add(JMenuItem("Run").apply { addActionListener { runSelected() } })
            if (isDebuggable(node)) {
                add(JMenuItem("Debug").apply { addActionListener { debugSelected(node) } })
            }
            add(JMenuItem("Open in cajeta.json").apply { addActionListener { openInManifest(node) } })
        }.show(e.component, e.x, e.y)
    }

    private inner class CajetaNodeRenderer : ColoredTreeCellRenderer() {
        override fun customizeCellRenderer(
            tree: JTree, value: Any?, selected: Boolean, expanded: Boolean,
            leaf: Boolean, row: Int, hasFocus: Boolean,
        ) {
            when (val uo = (value as? DefaultMutableTreeNode)?.userObject) {
                is TaskTreeNode -> {
                    append(uo.label)
                    uo.tooltip?.let {
                        append("  $it", SimpleTextAttributes.GRAYED_ATTRIBUTES)
                        toolTipText = it
                    }
                }
                is String -> append(uo, SimpleTextAttributes.REGULAR_BOLD_ATTRIBUTES)
                else -> {}
            }
        }
    }
}
