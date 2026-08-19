package dev.cajeta.idea.coverage

import com.intellij.openapi.project.DumbAware
import com.intellij.openapi.project.Project
import com.intellij.openapi.wm.ToolWindow
import com.intellij.openapi.wm.ToolWindowFactory
import com.intellij.ui.content.ContentFactory

/**
 * The coco tool window — everything IntelliJ's coverage model structurally
 * cannot express (spec §6, resolving §9.1).
 *
 * **One window, a tab per capability** (decided 2026-08-18). The four views —
 * dead-vs-untested, per-test attribution, the CRAP queue, mutation survivors —
 * are one subject looked at four ways, so they share a place, a toolbar and an
 * open/closed state. Separate windows would have crowded the sidebar with four
 * entries that are always wanted together; a single tree would have had to hold
 * four genuinely different column sets.
 *
 * Units 7 and 8 add tabs here rather than new windows.
 */
class CocoToolWindowFactory : ToolWindowFactory, DumbAware {

    /** Pointless without a project that measures coverage. */
    override fun shouldBeAvailable(project: Project): Boolean =
        CocoProject.of(project).isConfigured

    override fun createToolWindowContent(project: Project, toolWindow: ToolWindow) {
        val factory = ContentFactory.getInstance()
        toolWindow.contentManager.addContent(
            factory.createContent(CocoDeadCodePanel(project), "Dead Code", false)
        )
        toolWindow.contentManager.addContent(
            factory.createContent(CocoTestImpactPanel(project), "Tests", false)
        )
        toolWindow.contentManager.addContent(
            factory.createContent(CocoRiskPanel(project), "Risk", false)
        )
        toolWindow.contentManager.addContent(
            factory.createContent(CocoMutantsPanel(project), "Mutants", false)
        )
    }
}
