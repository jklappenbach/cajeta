package dev.cajeta.idea.coverage

import com.intellij.openapi.project.Project
import com.intellij.openapi.wm.ToolWindowManager

/**
 * Moving between the coco tabs, and landing on the row that was asked for.
 *
 * A cross-tab action that merely switches tabs has handed the reader a second
 * list to search. The point of "which tests cover this line" is the ANSWER, so
 * the destination tab selects the matching rows and scrolls to them; if it
 * cannot, the caller says so rather than switching to an unchanged list and
 * leaving the reader to wonder what happened.
 */
object CocoTabs {

    const val DEAD_CODE = "Dead Code"
    const val TESTS = "Tests"
    const val RISK = "Risk"
    const val MUTANTS = "Mutants"

    /** A panel that can be told which of its rows the caller means. */
    interface Selectable {
        /** Select every row matching [match]; returns how many were selected. */
        fun selectMatching(match: (Any) -> Boolean): Int
    }

    /**
     * Show [tab] and select the rows it holds matching [match].
     *
     * Returns the number selected, or -1 when the tab could not be reached at
     * all (no tool window, or it is not registered) — a caller reporting "3
     * tests" and a caller reporting "could not open the Tests tab" are saying
     * different things and must not be collapsed into 0.
     */
    fun revealIn(project: Project, tab: String, match: (Any) -> Boolean): Int {
        val toolWindow = ToolWindowManager.getInstance(project)
            .getToolWindow("Cajeta Coverage") ?: return -1
        val content = toolWindow.contentManager.contents
            .firstOrNull { it.displayName == tab } ?: return -1
        toolWindow.contentManager.setSelectedContent(content, true)
        toolWindow.show(null)
        val panel = content.component as? Selectable ?: return -1
        return panel.selectMatching(match)
    }
}
