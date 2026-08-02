package dev.cajeta.idea.buildtool

import com.intellij.icons.AllIcons
import com.intellij.openapi.actionSystem.ActionManager
import com.intellij.openapi.actionSystem.ActionToolbar
import com.intellij.openapi.actionSystem.AnAction
import com.intellij.openapi.actionSystem.AnActionEvent
import com.intellij.openapi.actionSystem.DefaultActionGroup
import com.intellij.openapi.actionSystem.ToggleAction
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.fileEditor.OpenFileDescriptor
import com.intellij.openapi.options.ShowSettingsUtil
import com.intellij.openapi.progress.ProgressIndicator
import com.intellij.openapi.progress.Task
import com.intellij.openapi.project.Project
import com.intellij.openapi.ui.SimpleToolWindowPanel
import com.intellij.openapi.ui.popup.JBPopupFactory
import com.intellij.openapi.vfs.LocalFileSystem
import com.intellij.openapi.vfs.VirtualFileManager
import com.intellij.openapi.vfs.newvfs.BulkFileListener
import com.intellij.openapi.vfs.newvfs.events.VFileEvent
import com.intellij.ui.ColoredTreeCellRenderer
import com.intellij.ui.EditorNotificationPanel
import com.intellij.util.Alarm
import com.intellij.ui.SimpleTextAttributes
import com.intellij.ui.TreeSpeedSearch
import com.intellij.ui.components.JBScrollPane
import com.intellij.ui.treeStructure.Tree
import com.intellij.util.ui.tree.TreeUtil
import dev.cajeta.idea.settings.CajetaConfigurable
import dev.cajeta.idea.settings.CajetaSettings
import java.awt.event.KeyEvent
import java.awt.event.MouseAdapter
import java.awt.event.MouseEvent
import java.io.File
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
     *  renderer both read cleanly. Carries its owning root manifest so per-root
     *  run/debug context is unambiguous in a multi-root tree (§10.2). */
    private class TaskLeaf(val data: TaskTreeNode, val manifestPath: String) : DefaultMutableTreeNode(data) {
        override fun toString(): String = data.label
    }

    /** A linked-root header node (shown when >1 root); carries the root manifest
     *  so Unlink and per-root discovery can target it. */
    private class RootNode(val manifestPath: String, label: String) : DefaultMutableTreeNode(label)

    /** Discovered model per root manifest path (§10.2.1: each root its own tasks).
     *  `lastModel` mirrors the primary root for the existing single-root paths. */
    private val rootModels = LinkedHashMap<String, TaskModel>()
    private var lastModel: TaskModel? = null

    /** A reload banner (spec §13.2.1): shown when a watched manifest changes and
     *  auto-reload is "prompt"; its Reload action re-runs discovery. */
    private val reloadBanner = EditorNotificationPanel().apply {
        text("Cajeta projects need to be reloaded")
        createActionLabel("Reload") { hideBanner(); reload() }
        isVisible = false
    }
    private val contentPanel = javax.swing.JPanel(java.awt.BorderLayout()).apply {
        add(reloadBanner, java.awt.BorderLayout.NORTH)
    }
    private val reloadAlarm = Alarm(Alarm.ThreadToUse.SWING_THREAD, project)

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
        contentPanel.add(JBScrollPane(tree), java.awt.BorderLayout.CENTER)
        setContent(contentPanel)
        setToolbar(buildToolbarComponent())
        installManifestWatcher()
        reload()
    }

    private fun showBanner() { reloadBanner.isVisible = true; contentPanel.revalidate() }
    private fun hideBanner() { reloadBanner.isVisible = false; contentPanel.revalidate() }

    /** Watch the linked `cajeta.json` files; on change, decide per the auto-reload
     *  setting (spec §13): reload (debounced), prompt via banner, or ignore. */
    private fun installManifestWatcher() {
        val roots = allRoots().toSet()
        project.messageBus.connect(project).subscribe(
            VirtualFileManager.VFS_CHANGES,
            object : BulkFileListener {
                override fun after(events: List<VFileEvent>) {
                    if (events.none { it.path in roots }) return
                    when (AutoSync.decide(CajetaSettings.instance.buildAutoReload)) {
                        AutoSync.Action.IGNORE -> {}
                        AutoSync.Action.PROMPT -> showBanner()
                        AutoSync.Action.RELOAD -> {
                            reloadAlarm.cancelAllRequests()
                            reloadAlarm.addRequest({ reload() }, DEBOUNCE_MS)
                        }
                    }
                }
            },
        )
    }

    /** Toolbar (actions) + an active profile/flavor selector (spec §12.1) that
     *  edits the defaults double-click runs use, so changing them retargets
     *  subsequent runs. */
    private fun buildToolbarComponent(): JComponent {
        // Editable combo, filled from the compiler's own answer (--list-profiles).
        // Editable is the whole safety of it: discovery suggests, and a profile
        // the query never saw is still typeable. Until the answer arrives — or
        // if it never does — this behaves exactly like the text field it
        // replaced.
        val profileSelector = com.intellij.openapi.ui.ComboBox<String>().apply {
            isEditable = true
            toolTipText = "Active --profile for runs"
            addItem(CajetaProfileCandidates.DEFAULT_PROFILE)
            // What this configuration was last set to, else the global default
            // until discovery supplies a better one.
            val remembered = CajetaProfileMemory.getInstance(project)
                .profileFor(CajetaProfileMemory.currentConfigurationName(project))
            val current = remembered ?: CajetaSettings.instance.defaultProfile
            editor?.item = current
            selectedItem = current
            addActionListener {
                val text = (editor?.item ?: selectedItem)?.toString()?.trim() ?: ""
                // Both: the per-configuration memory is what restores this
                // selector, the global default is what launches still read.
                CajetaProfileMemory.getInstance(project).remember(
                    CajetaProfileMemory.currentConfigurationName(project), text)
                CajetaSettings.instance.defaultProfile = text
            }
        }
        loadProfilesInto(profileSelector)
        val flavorSelector = javax.swing.JTextField(CajetaSettings.instance.defaultFlavor, 8).apply {
            toolTipText = "Active flavor (--release/--debug/--fast or a name)"
            addActionListener { CajetaSettings.instance.defaultFlavor = text.trim() }
        }
        val selectors = javax.swing.JPanel().apply {
            add(javax.swing.JLabel("Profile:")); add(profileSelector)
            add(javax.swing.JLabel("Flavor:")); add(flavorSelector)
        }
        return javax.swing.JPanel(java.awt.BorderLayout()).apply {
            add(buildToolbar().component, java.awt.BorderLayout.WEST)
            add(selectors, java.awt.BorderLayout.EAST)
        }
    }

    /** Current toolbar enable/disable + grouping state (spec §9), computed from
     *  the discovered model, selection, and active runs. */
    private fun toolbarState(): BuildToolbarState = BuildToolbarState(
        hasTasks = (lastModel?.tasks?.isNotEmpty() == true) || (lastModel?.builtins?.isNotEmpty() == true),
        activeRuns = BuildRunTracker.getInstance(project).activeCount(),
        selectionRunnable = selectedTask()?.kind == TaskTreeNode.Kind.TASK,
        groupByProject = CajetaSettings.instance.buildGroupByProject,
    )

    private fun buildToolbar(): ActionToolbar {
        val group = DefaultActionGroup().apply {
            add(action("Refresh", "Re-run task discovery", AllIcons.Actions.Refresh,
                { true }) { reload() })
            add(action("Run Task…", "Pick a task to run", AllIcons.Actions.Execute,
                { toolbarState().runTaskPickerEnabled }) { showRunTaskPicker() })
            add(action("Stop", "Stop active runs", AllIcons.Actions.Suspend,
                { toolbarState().stopEnabled }) { BuildRunTracker.getInstance(project).stopAll() })
            addSeparator()
            add(action("Expand All", "Expand all nodes", AllIcons.Actions.Expandall,
                { toolbarState().expandCollapseEnabled }) { TreeUtil.expandAll(tree) })
            add(action("Collapse All", "Collapse all nodes", AllIcons.Actions.Collapseall,
                { toolbarState().expandCollapseEnabled }) { TreeUtil.collapseAll(tree, 1) })
            add(object : ToggleAction("Group by Project", "Group the tree by project root", AllIcons.Actions.GroupByModule) {
                override fun isSelected(e: AnActionEvent) = CajetaSettings.instance.buildGroupByProject
                override fun setSelected(e: AnActionEvent, state: Boolean) {
                    CajetaSettings.instance.buildGroupByProject = state
                    if (rootModels.isNotEmpty()) rebuildTree()
                }
            })
            addSeparator()
            add(action("Link Project…", "Link another cajeta.json root", AllIcons.General.Add,
                { true }) { linkProject() })
            add(action("Settings", "Open Cajeta settings", AllIcons.General.Settings, { true }) {
                ShowSettingsUtil.getInstance().showSettingsDialog(project, CajetaConfigurable::class.java)
            })
        }
        return ActionManager.getInstance().createActionToolbar("CajetaBuildToolbar", group, true).apply {
            targetComponent = this@CajetaBuildToolPanel
        }
    }

    private fun action(
        text: String,
        description: String,
        icon: javax.swing.Icon,
        enabled: () -> Boolean,
        run: () -> Unit,
    ): AnAction = object : AnAction(text, description, icon) {
        override fun update(e: AnActionEvent) { e.presentation.isEnabled = enabled() }
        override fun actionPerformed(e: AnActionEvent) = run()
    }

    private fun showRunTaskPicker() {
        val tasks = lastModel?.tasks?.map { it.name }?.sorted().orEmpty()
        if (tasks.isEmpty()) return
        JBPopupFactory.getInstance()
            .createPopupChooserBuilder(tasks)
            .setTitle("Run Cajeta Task")
            .setItemChosenCallback { name ->
                val manifest = CajetaManifest.path(project) ?: return@setItemChosenCallback
                val model = rootModels[manifest] ?: lastModel ?: return@setItemChosenCallback
                CajetaTaskLauncher.launch(project, manifest, TaskTreeNode(name, null, TaskTreeNode.Kind.TASK), model)
            }
            .createPopup()
            .showInFocusCenter()
    }

    /** All `cajeta.json` roots in play (spec §10): the auto-detected project
     *  root, the manually linked roots, and any workspace's member manifests. */
    private fun allRoots(): List<String> {
        val roots = LinkedHashSet<String>()
        CajetaManifest.path(project)?.let { roots += it }
        roots += LinkedRootsService.getInstance(project).linkedPaths()
        // Expand workspace members (§10.2.4) of every root we know.
        for (r in roots.toList()) {
            val text = runCatching { File(r).readText() }.getOrNull() ?: continue
            roots += CajetaRoots.workspaceMembers(text, File(r).parent ?: ".")
        }
        return roots.toList()
    }

    /** Re-run discovery for every root and rebuild the tree (§10.2.1). */
    fun reload() {
        val roots = allRoots()
        if (roots.isEmpty()) {
            showMessage("No cajeta.json in this project")
            return
        }
        object : Task.Backgroundable(project, "Discovering Cajeta tasks", true) {
            override fun run(indicator: ProgressIndicator) {
                val discovered = LinkedHashMap<String, TaskModel>()
                val failures = mutableListOf<String>()
                for (root in roots) {
                    when (val r = CajetaBuildRunner.discover(CajetaSettings.instance.buildToolPath, root)) {
                        is CajetaBuildRunner.DiscoverResult.Ok -> discovered[root] = r.model
                        is CajetaBuildRunner.DiscoverResult.Failed -> failures += "${File(root).parent}: ${r.reason}"
                    }
                }
                ApplicationManager.getApplication().invokeLater {
                    hideBanner()
                    // §13.2.3: a failed re-discovery keeps the prior tree usable.
                    val next = AutoSync.reconcile(LinkedHashMap(rootModels), discovered)
                    if (next.isEmpty()) {
                        showMessage(failures.joinToString("\n").ifBlank { "Discovery failed" })
                        return@invokeLater
                    }
                    rootModels.clear()
                    rootModels.putAll(next)
                    lastModel = rootModels[CajetaManifest.path(project)] ?: rootModels.values.firstOrNull()
                    rebuildTree()
                }
            }
        }.queue()
    }

    private fun rebuildTree() {
        rootNode.removeAllChildren()
        addFavoritesGroup()
        val multi = rootModels.size > 1
        for ((manifest, model) in rootModels) {
            val parent = if (multi) RootNode(manifest, File(manifest).parentFile?.name ?: manifest).also { rootNode.add(it) } else rootNode
            addGroups(parent, model, manifest)
        }
        treeModel.reload()
        TreeUtil.expandAll(tree)
    }

    /** Favorites pinned at the very top of the tree (spec §11.2.3), resolved
     *  against the loaded root models. */
    private fun addFavoritesGroup() {
        val favs = FavoritesService.getInstance(project).list()
        if (favs.isEmpty()) return
        val groupNode = DefaultMutableTreeNode(GROUP_FAVORITES)
        var any = false
        for (ref in favs) {
            val task = rootModels[ref.manifestPath]?.tasks?.firstOrNull { it.name == ref.task } ?: continue
            groupNode.add(TaskLeaf(TaskTreeNode(task.name, task.description?.takeIf(String::isNotBlank), TaskTreeNode.Kind.TASK), ref.manifestPath))
            any = true
        }
        if (any) rootNode.add(groupNode)
    }

    private fun addGroups(parent: DefaultMutableTreeNode, model: TaskModel, manifest: String) {
        val groups = TaskTreeModelBuilder.build(model)
        if (CajetaSettings.instance.buildGroupByProject) {
            for (group in groups) {
                val groupNode = DefaultMutableTreeNode(group.title)
                for (node in group.nodes) groupNode.add(TaskLeaf(node, manifest))
                parent.add(groupNode)
            }
        } else {
            // Flat presentation (§9.2.4): leaves directly under the parent.
            for (group in groups) for (node in group.nodes) parent.add(TaskLeaf(node, manifest))
        }
    }

    private fun showMessage(text: String) {
        rootNode.removeAllChildren()
        rootNode.add(DefaultMutableTreeNode(text))
        treeModel.reload()
    }

    private fun selectedLeaf(): TaskLeaf? = tree.selectionPath?.lastPathComponent as? TaskLeaf
    private fun selectedTask(): TaskTreeNode? = selectedLeaf()?.data
    private fun selectedRoot(): RootNode? = tree.selectionPath?.lastPathComponent as? RootNode

    private fun runSelected() {
        val leaf = selectedLeaf() ?: return
        val model = rootModels[leaf.manifestPath] ?: return
        CajetaTaskLauncher.launch(project, leaf.manifestPath, leaf.data, model)
    }

    private fun debugSelected(leaf: TaskLeaf) {
        val model = rootModels[leaf.manifestPath] ?: return
        CajetaTaskLauncher.debug(project, leaf.manifestPath, model, leaf.data)
    }

    /** Open the Run-with-args dialog for the task and run it with the result
     *  (spec §12.2). */
    private fun runWithArgs(leaf: TaskLeaf) {
        val model = rootModels[leaf.manifestPath] ?: return
        val task = model.tasks.firstOrNull { it.name == leaf.data.runName } ?: return
        val dialog = RunWithArgsDialog(
            project, task, leaf.manifestPath,
            CajetaSettings.instance.defaultProfile, CajetaSettings.instance.defaultFlavor,
        )
        if (dialog.showAndGet()) CajetaTaskLauncher.launchWithSpec(project, dialog.result(), leaf.data, model)
    }

    /** Whether the leaf maps to a dap-debuggable task (§5.2.2). */
    private fun isDebuggable(leaf: TaskLeaf): Boolean {
        if (leaf.data.kind != TaskTreeNode.Kind.TASK) return false
        val model = rootModels[leaf.manifestPath] ?: return false
        val task = model.tasks.firstOrNull { it.name == leaf.data.runName } ?: return false
        return TaskDebugMapping.isDebuggable(task, model)
    }

    /** Persist the task as a saved run configuration bound to its active
     *  bindings (spec §11.2.1). */
    private fun saveAsConfig(leaf: TaskLeaf) {
        val task = rootModels[leaf.manifestPath]?.tasks?.firstOrNull { it.name == leaf.data.runName } ?: return
        val spec = SavedConfig.specFor(
            task, leaf.manifestPath,
            CajetaSettings.instance.defaultProfile, CajetaSettings.instance.defaultFlavor,
        )
        CajetaTaskLauncher.saveConfig(project, spec)
    }

    private fun toggleFavorite(leaf: TaskLeaf) {
        FavoritesService.getInstance(project).toggle(FavoriteRef(leaf.manifestPath, leaf.data.runName))
        rebuildTree()
    }

    private fun openInManifest(leaf: TaskLeaf) {
        val vf = LocalFileSystem.getInstance().findFileByPath(leaf.manifestPath) ?: return
        val text = runCatching {
            String(vf.contentsToByteArray(), StandardCharsets.UTF_8)
        }.getOrDefault("")
        val offset = ManifestTaskLocator.offsetOf(text, leaf.data.runName) ?: 0
        OpenFileDescriptor(project, vf, offset).navigate(true)
    }

    /** Pick a `cajeta.json` to link as an additional root (§10.2.2). */
    private fun linkProject() {
        val descriptor = com.intellij.openapi.fileChooser.FileChooserDescriptor(true, false, false, false, false, false)
            .withFileFilter { it.name == "cajeta.json" }
            .withTitle("Link Cajeta Project")
        com.intellij.openapi.fileChooser.FileChooser.chooseFile(descriptor, project, null)?.let { chosen ->
            LinkedRootsService.getInstance(project).link(chosen.path)
            reload()
        }
    }

    private fun unlink(root: RootNode) {
        LinkedRootsService.getInstance(project).unlink(root.manifestPath)
        reload()
    }

    private fun showContextMenu(e: MouseEvent) {
        selectedRoot()?.let { root ->
            // A linked (non-primary) root can be unlinked (§10.2.3).
            if (root.manifestPath != CajetaManifest.path(project)) {
                JPopupMenu().apply {
                    add(JMenuItem("Unlink Project").apply { addActionListener { unlink(root) } })
                }.show(e.component, e.x, e.y)
            }
            return
        }
        val leaf = selectedLeaf() ?: return
        JPopupMenu().apply {
            add(JMenuItem("Run").apply { addActionListener { runSelected() } })
            if (leaf.data.kind == TaskTreeNode.Kind.TASK) {
                add(JMenuItem("Run with Arguments…").apply { addActionListener { runWithArgs(leaf) } })
            }
            if (isDebuggable(leaf)) {
                add(JMenuItem("Debug").apply { addActionListener { debugSelected(leaf) } })
            }
            if (leaf.data.kind == TaskTreeNode.Kind.TASK) {
                add(JMenuItem("Save as Run Configuration").apply { addActionListener { saveAsConfig(leaf) } })
                val fav = FavoritesService.getInstance(project).contains(FavoriteRef(leaf.manifestPath, leaf.data.runName))
                add(JMenuItem(if (fav) "Remove from Favorites" else "Add to Favorites").apply {
                    addActionListener { toggleFavorite(leaf) }
                })
            }
            add(JMenuItem("Open in cajeta.json").apply { addActionListener { openInManifest(leaf) } })
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

    companion object {
        private const val GROUP_FAVORITES = "Favorites"
        /** Coalesce a burst of manifest edits into one reload (spec §13.1). */
        private const val DEBOUNCE_MS = 400
    }

    /**
     * Fill the profile selector from `cajeta --lint <root> --list-profiles`,
     * off the EDT. Every failure — no compiler configured, no manifest yet
     * discovered, a non-zero exit, unreadable output — leaves the selector as
     * the editable field it already is, so nothing here can stop a build being
     * launched. What the developer has typed is never replaced.
     */
    private fun loadProfilesInto(selector: com.intellij.openapi.ui.ComboBox<String>) {
        val compiler = CajetaSettings.instance.buildToolPath
        if (compiler.isBlank()) return
        com.intellij.openapi.application.ApplicationManager.getApplication()
            .executeOnPooledThread {
                val root = profileQueryRoot() ?: return@executeOnPooledThread
                val result = try {
                    val proc = ProcessBuilder(
                        CajetaProfileCandidates.argvFor(compiler, root)).start()
                    val out = proc.inputStream.bufferedReader().readText()
                    proc.waitFor()
                    CajetaProfileCandidates.parse(out)
                } catch (t: Throwable) {
                    CajetaProfileCandidates.Result(emptyList(), queried = false,
                        error = t.message)
                }
                com.intellij.openapi.application.ApplicationManager.getApplication()
                    .invokeLater {
                        val typed = (selector.editor?.item ?: selector.selectedItem)
                            ?.toString()?.trim() ?: ""
                        selector.removeAllItems()
                        for (p in result.offered()) selector.addItem(p)
                        // Precedence: what this configuration remembers, then
                        // whatever was already in the box, then the FIRST
                        // discovered profile. Discovery only fills a vacuum —
                        // it never overrides a choice already made.
                        val config = CajetaProfileMemory.currentConfigurationName(project)
                        val chosen = CajetaProfileMemory.getInstance(project)
                            .profileFor(config)
                            ?: typed.ifBlank { null }
                            ?: result.defaultSelection()
                        selector.editor?.item = chosen
                        selector.selectedItem = chosen
                        if (CajetaSettings.instance.defaultProfile.isBlank())
                            CajetaSettings.instance.defaultProfile = chosen
                        selector.toolTipText =
                            result.emptyMessage() ?: "Active --profile for runs"
                    }
            }
    }

    /** The source root to ask about: the first discovered manifest's directory,
     *  falling back to the project base. */
    private fun profileQueryRoot(): String? {
        val manifest = rootModels.keys.firstOrNull()
        val dir = if (manifest != null) java.io.File(manifest).parentFile
                  else project.basePath?.let { java.io.File(it) }
        if (dir == null || !dir.isDirectory) return null
        val src = java.io.File(dir, "src")
        return if (src.isDirectory) src.absolutePath else dir.absolutePath
    }
}
