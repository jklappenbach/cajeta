package dev.cajeta.idea.buildtool

import com.intellij.openapi.project.DumbAware
import com.intellij.openapi.project.Project
import com.intellij.openapi.wm.ToolWindow
import com.intellij.openapi.wm.ToolWindowFactory
import com.intellij.ui.content.ContentFactory

/**
 * Registers the Cajeta build-tool tool window, docked like Gradle's (spec §2).
 * Available only when the project has a `cajeta.json` (§2.2.1/2.2.3) — gated via
 * [shouldBeAvailable].
 */
class CajetaBuildToolWindowFactory : ToolWindowFactory, DumbAware {

    override fun shouldBeAvailable(project: Project): Boolean = CajetaManifest.exists(project)

    override fun createToolWindowContent(project: Project, toolWindow: ToolWindow) {
        val panel = CajetaBuildToolPanel(project)
        val content = ContentFactory.getInstance().createContent(panel, "", false)
        toolWindow.contentManager.addContent(content)
    }
}
