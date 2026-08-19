package dev.cajeta.idea.coverage

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
class CocoMutantsPanel(private val project: Project) : JPanel(BorderLayout()) {

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
}
