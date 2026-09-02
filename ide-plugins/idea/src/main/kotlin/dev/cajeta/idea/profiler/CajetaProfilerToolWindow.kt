package dev.cajeta.idea.profiler

import com.intellij.openapi.actionSystem.ActionUpdateThread
import com.intellij.openapi.actionSystem.AnAction
import com.intellij.openapi.actionSystem.AnActionEvent
import com.intellij.openapi.fileChooser.FileChooser
import com.intellij.openapi.fileChooser.FileChooserDescriptor
import com.intellij.openapi.project.DumbAware
import com.intellij.openapi.project.Project
import com.intellij.openapi.wm.ToolWindow
import com.intellij.openapi.wm.ToolWindowManager
import com.intellij.ui.content.ContentFactory
import java.io.File

/**
 * cajeta-profiler 11.2.e — the tool window (spec §8.1).
 *
 * One window with a tab per view, following the coco window's precedent.
 */
class CajetaProfilerToolWindowFactory : com.intellij.openapi.wm.ToolWindowFactory, DumbAware {

    override fun createToolWindowContent(project: Project, toolWindow: ToolWindow) {
        val panel = CajetaProfilerPanel(project)
        CajetaProfilerToolWindow.register(project, panel)
        toolWindow.contentManager.addContent(
            ContentFactory.getInstance().createContent(panel, "Profile", false)
        )
    }
}

/**
 * The bridge between "a trace exists" and "the window is showing it".
 *
 * Held per project so an action, or a run configuration that has just finished,
 * can hand a file to a window that may not have been created yet.
 */
object CajetaProfilerToolWindow {

    const val ID = "Cajeta Profiler"

    private val panels = HashMap<Project, CajetaProfilerPanel>()
    private val pending = HashMap<Project, File>()

    internal fun register(project: Project, panel: CajetaProfilerPanel) {
        panels[project] = panel
        // A trace handed over before the window existed is not dropped: the run
        // that produced it has already finished, and asking the developer to go
        // find the file again would waste the one moment they wanted it.
        pending.remove(project)?.let { panel.load(it) }
    }

    fun open(project: Project, trace: File) {
        val window = ToolWindowManager.getInstance(project).getToolWindow(ID)
        val panel = panels[project]
        if (panel == null) {
            pending[project] = trace
            window?.activate(null)
            return
        }
        window?.activate { panel.load(trace) } ?: panel.load(trace)
    }

    internal fun forget(project: Project) {
        panels.remove(project)
        pending.remove(project)
    }
}

/**
 * cajeta-profiler 11.2.e — "Open Cajeta Profile" (spec §8.1).
 *
 * The default directory is where the runtime writes when `CAJETA_PROFILER_OUT`
 * is unset (§9.4). The platform's generic Open would ask the developer to find
 * a file the toolchain already knows the location of.
 */
class OpenCajetaProfileAction : AnAction(), DumbAware {

    override fun getActionUpdateThread(): ActionUpdateThread = ActionUpdateThread.BGT

    override fun update(e: AnActionEvent) {
        e.presentation.isEnabled = e.project != null
    }

    override fun actionPerformed(e: AnActionEvent) {
        val project = e.project ?: return
        val chosen = CajetaProfileLocation.choose(project) ?: return
        CajetaProfilerToolWindow.open(project, chosen)
    }
}
