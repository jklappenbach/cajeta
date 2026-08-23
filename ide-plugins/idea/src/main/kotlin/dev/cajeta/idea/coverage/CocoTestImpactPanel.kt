package dev.cajeta.idea.coverage

import com.intellij.openapi.actionSystem.ActionManager
import com.intellij.openapi.actionSystem.ActionPlaces
import com.intellij.openapi.actionSystem.DefaultActionGroup
import com.intellij.openapi.actionSystem.AnActionEvent
import com.intellij.openapi.project.DumbAwareAction
import com.intellij.ui.PopupHandler
import com.intellij.openapi.project.Project
import com.intellij.ui.ColoredListCellRenderer
import com.intellij.ui.SimpleTextAttributes
import com.intellij.ui.components.JBLabel
import com.intellij.ui.components.JBList
import com.intellij.ui.components.JBScrollPane
import java.awt.BorderLayout
import javax.swing.DefaultListModel
import javax.swing.JPanel

/**
 * Per-test attribution (spec §6.2): what each test uniquely contributes.
 *
 * Redundancy is shown as a CANDIDATE, never a verdict. A test can be worth
 * keeping for what it asserts even when another test happens to execute the same
 * lines — coverage overlap is evidence, not proof, and a view that said
 * "delete this test" on that basis would be wrong often enough to be dangerous.
 */
class CocoTestImpactPanel(private val project: Project) : JPanel(BorderLayout()), CocoTabs.Selectable {

    private val model = DefaultListModel<CocoTestSummary>()
    private val list = JBList(model)
    private val status = JBLabel(" ")

    init {
        list.cellRenderer = object : ColoredListCellRenderer<CocoTestSummary>() {
            override fun customizeCellRenderer(
                list: javax.swing.JList<out CocoTestSummary>,
                value: CocoTestSummary,
                index: Int,
                selected: Boolean,
                hasFocus: Boolean,
            ) {
                append(value.name, SimpleTextAttributes.REGULAR_ATTRIBUTES)
                append("   ${describe(value)}", SimpleTextAttributes.GRAYED_ATTRIBUTES)
            }
        }

        val group = DefaultActionGroup().apply {
            add(CocoCrossActions.SurvivorsForTest(project) { list.selectedValue })
        }
        PopupHandler.installPopupMenu(list, group, ActionPlaces.TOOLWINDOW_CONTENT)

        add(JBScrollPane(list), BorderLayout.CENTER)
        add(status, BorderLayout.SOUTH)

        CocoAnalysis.getInstance(project).addSideListener { refresh() }
    }

    private fun refresh() {
        val attribution = CocoAnalysis.getInstance(project).perTest
        model.clear()
        if (attribution == null) {
            // 6.2.4 — say the data was not collected rather than showing an
            // empty list, which reads as "no test covers anything".
            status.text = " ${CocoAttributionModel.NOT_COLLECTED}"
            return
        }
        attribution.tests.sortedBy { it.name }.forEach(model::addElement)
        status.text = summarize(attribution.redundancyCandidates().size, attribution.tests.size)
    }

    companion object {
        fun describe(t: CocoTestSummary): String =
            if (t.unique == 0L) "${t.covered} lines, none unique — redundancy candidate"
            else "${t.covered} lines, ${t.unique} unique"

        fun summarize(candidates: Int, total: Int): String = when {
            total == 0 -> " No tests attributed."
            candidates == 0 -> " $total tests, every one contributes something unique"
            else -> " $total tests, $candidates contribute no unique coverage " +
                "(candidates for review, not deletion)"
        }
    }
    /**
     * Select the rows a cross-tab action asked for, and scroll the first into
     * view. Returning the COUNT lets the caller distinguish "found none" from
     * "could not look" — an action that silently selects nothing reads as
     * broken.
     */
    override fun selectMatching(match: (Any) -> Boolean): Int {
        val indices = (0 until model.size()).filter { match(model.getElementAt(it) as Any) }
        list.selectedIndices = indices.toIntArray()
        indices.firstOrNull()?.let { list.ensureIndexIsVisible(it) }
        return indices.size
    }

}
