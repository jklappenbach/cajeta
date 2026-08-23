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
 * The CRAP queue (spec §6.3) — an open-ended hunt turned into an ordered list.
 *
 * The ranking is coco's and is shown in the order coco emitted it. Re-sorting
 * would be re-deciding the ranking, and the score itself is never recomputed on
 * this side (plan 7.3.a): one metric, one definition.
 *
 * Every row shows its inputs. A ranking nobody can interrogate gets ignored;
 * seeing that a score is high because complexity is 12 and coverage is 8% is
 * what makes it something to act on (spec §6.3.3).
 */
class CocoRiskPanel(private val project: Project) : JPanel(BorderLayout()) {

    private val model = DefaultListModel<CrapEntry>()
    private val list = JBList(model)
    private val status = JBLabel(" ")

    init {
        list.cellRenderer = object : ColoredListCellRenderer<CrapEntry>() {
            override fun customizeCellRenderer(
                list: javax.swing.JList<out CrapEntry>,
                value: CrapEntry,
                index: Int,
                selected: Boolean,
                hasFocus: Boolean,
            ) {
                append(
                    value.score.padStart(6),
                    if (value.isHighRisk) SimpleTextAttributes.ERROR_ATTRIBUTES
                    else SimpleTextAttributes.GRAYED_ATTRIBUTES,
                )
                append("  ${value.method}", SimpleTextAttributes.REGULAR_ATTRIBUTES)
                append("   ${value.explain()}", SimpleTextAttributes.GRAYED_ITALIC_ATTRIBUTES)
            }
        }
        // 6.3.2 — navigating to the method is one action.
        list.addMouseListener(object : MouseAdapter() {
            override fun mouseClicked(e: MouseEvent) {
                if (e.clickCount == 2) navigate()
            }
        })

        val group = DefaultActionGroup().apply {
            add(object : DumbAwareAction("Jump to Source") {
                override fun actionPerformed(e: AnActionEvent) = navigate()
            })
            add(CocoCrossActions.GoToFirstUncoveredLine(project) { list.selectedValue })
            add(CocoCrossActions.CopyExplanation { list.selectedValue })
        }
        PopupHandler.installPopupMenu(list, group, ActionPlaces.TOOLWINDOW_CONTENT)

        add(JBScrollPane(list), BorderLayout.CENTER)
        add(status, BorderLayout.SOUTH)

        CocoAnalysis.getInstance(project).addSideListener { refresh() }
    }

    private fun refresh() {
        val ranking = CocoAnalysis.getInstance(project).risk
        model.clear()
        // Absent is stated, never rendered as an empty queue — an empty list
        // reads as "nothing is risky", which is a different claim.
        status.text = if (ranking == null) " ${CocoCrap.NOT_AVAILABLE}" else summarize(ranking)
        ranking?.forEach(model::addElement)
    }

    private fun navigate() {
        val entry = list.selectedValue ?: return
        val coverage = CocoAnalysis.getInstance(project).coverage ?: return
        // The ranking names methods as owner.method; the site table is what
        // knows where that lives.
        val site = coverage.sites.firstOrNull {
            "${it.owner}.${it.method}" == entry.method && it.isSourceLine
        } ?: return
        CocoNavigation.open(project, site.file, site.line)
    }

    companion object {
        /** Separated so it is assertable without Swing. */
        fun summarize(ranking: List<CrapEntry>): String {
            if (ranking.isEmpty()) return " No methods ranked."
            val high = ranking.count { it.isHighRisk }
            return " ${ranking.size} methods ranked, $high above the CRAP threshold of 30"
        }
    }
}
