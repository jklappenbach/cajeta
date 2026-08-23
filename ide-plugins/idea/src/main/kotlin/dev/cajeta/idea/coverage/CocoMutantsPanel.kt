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
import java.awt.event.MouseAdapter
import java.awt.event.MouseEvent
import javax.swing.DefaultListModel
import javax.swing.JPanel

/**
 * Surviving mutants (spec §6.4).
 *
 * Only survivors are listed. A killed mutant is the suite working, and an
 * uncovered one is already reported by every other view here — listing either
 * would bury the two rows that mean something.
 *
 * A survivor is the most specific finding this tool produces: not "this line is
 * untested" but "this line ran, its behaviour was changed, and nothing
 * complained". That is execution without verification, and it is invisible to
 * coverage alone.
 */
class CocoMutantsPanel(private val project: Project) : JPanel(BorderLayout()), CocoTabs.Selectable {

    private val model = DefaultListModel<MutantResult>()
    private val list = JBList(model)
    private val status = JBLabel(" ")

    init {
        list.cellRenderer = object : ColoredListCellRenderer<MutantResult>() {
            override fun customizeCellRenderer(
                list: javax.swing.JList<out MutantResult>,
                value: MutantResult,
                index: Int,
                selected: Boolean,
                hasFocus: Boolean,
            ) {
                append("SURVIVED", SimpleTextAttributes.ERROR_ATTRIBUTES)
                append("  ${value.mutation}", SimpleTextAttributes.REGULAR_ATTRIBUTES)
                append("  ${value.sourceFile}:${value.srcLine}", SimpleTextAttributes.GRAYED_ATTRIBUTES)
                append("  ${value.method}", SimpleTextAttributes.GRAYED_ITALIC_ATTRIBUTES)
            }
        }
        // 6.4.2 — navigating to the mutated site is one action.
        list.addMouseListener(object : MouseAdapter() {
            override fun mouseClicked(e: MouseEvent) {
                if (e.clickCount == 2) {
                    val m = list.selectedValue ?: return
                    CocoNavigation.open(project, m.sourceFile, m.srcLine)
                }
            }
        })

        // Context actions. Double-click already navigated but nothing said so;
        // a list of findings invites a right-click, and finding nothing there
        // reads as a list that does nothing.
        val group = DefaultActionGroup().apply {
            add(object : DumbAwareAction("Jump to Source") {
                override fun actionPerformed(e: AnActionEvent) {
                    val m = list.selectedValue ?: return
                    CocoNavigation.open(project, m.sourceFile, m.srcLine)
                }
            })
            add(CocoCrossActions.TestsCoveringMutant(project) { list.selectedValue })
        }
        PopupHandler.installPopupMenu(list, group, ActionPlaces.TOOLWINDOW_CONTENT)

        add(JBScrollPane(list), BorderLayout.CENTER)
        add(status, BorderLayout.SOUTH)

        CocoAnalysis.getInstance(project).addSideListener { refresh() }
    }

    private fun refresh() {
        val all = CocoAnalysis.getInstance(project).mutants
        model.clear()
        if (all == null) {
            // 8.1.d — say why there is nothing, rather than showing an empty
            // list that reads as "no mutants survived".
            status.text = " ${CocoMutation.NOT_AVAILABLE}"
            return
        }
        CocoMutation.survivors(all).forEach(model::addElement)
        status.text = CocoMutation.summarize(all)
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
