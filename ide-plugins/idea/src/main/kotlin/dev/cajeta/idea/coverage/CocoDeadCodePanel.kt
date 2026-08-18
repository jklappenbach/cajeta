package dev.cajeta.idea.coverage

import com.intellij.openapi.actionSystem.ActionManager
import com.intellij.openapi.actionSystem.ActionPlaces
import com.intellij.openapi.actionSystem.AnActionEvent
import com.intellij.openapi.actionSystem.DefaultActionGroup
import com.intellij.openapi.project.DumbAwareAction
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
 * The dead-vs-untested tab (spec §6.1).
 *
 * The two verdicts are presented as different KINDS of finding, not two shades
 * of the same red: one is work to add, the other is work to remove. That
 * distinction is the reason this view exists at all — an lcov report renders
 * both identically.
 *
 * Undetermined entries are listed too, and say so. Hiding them would quietly
 * shrink the problem; defaulting them into either bucket would be a claim the
 * analysis did not make (spec §6.1.5).
 */
class CocoDeadCodePanel(private val project: Project) : JPanel(BorderLayout()) {

    private val model = DefaultListModel<UncoveredMethod>()
    private val list = JBList(model)
    private val status = JBLabel(" No coverage loaded.")

    init {
        list.cellRenderer = Renderer()
        list.addMouseListener(object : MouseAdapter() {
            override fun mouseClicked(e: MouseEvent) {
                if (e.clickCount == 2) navigateToSelection()
            }
        })

        val group = DefaultActionGroup().apply {
            add(object : DumbAwareAction("Jump to Source") {
                override fun actionPerformed(e: AnActionEvent) = navigateToSelection()
            })
            // §9.2, decided 2026-08-18: Safe Delete rather than a raw delete.
            // The platform runs its OWN usage search and blocks on conflicts, so
            // a false positive here gets an independent second opinion before
            // anything is removed. Nothing is ever deleted silently.
            add(CocoSafeDeleteAction { selection() })
        }
        val toolbar = ActionManager.getInstance()
            .createActionToolbar(ActionPlaces.TOOLWINDOW_CONTENT, group, true)
        toolbar.targetComponent = list

        add(toolbar.component, BorderLayout.NORTH)
        add(JBScrollPane(list), BorderLayout.CENTER)
        add(status, BorderLayout.SOUTH)

        // Subscribing (rather than reading once) covers the normal order:
        // results land first, and someone opens the tab afterwards.
        CocoAnalysis.getInstance(project).addListener { showFindings(it) }
    }

    fun showFindings(findings: List<UncoveredMethod>) {
        model.clear()
        findings.forEach(model::addElement)
        status.text = summarize(findings)
    }

    private fun selection(): UncoveredMethod? = list.selectedValue

    private fun navigateToSelection() {
        val m = selection() ?: return
        CocoNavigation.open(project, m.file, m.line)
    }

    private class Renderer : ColoredListCellRenderer<UncoveredMethod>() {
        override fun customizeCellRenderer(
            list: javax.swing.JList<out UncoveredMethod>,
            value: UncoveredMethod,
            index: Int,
            selected: Boolean,
            hasFocus: Boolean,
        ) {
            append(labelOf(value.verdict), SimpleTextAttributes.GRAYED_BOLD_ATTRIBUTES)
            append("  ${value.displayName}", SimpleTextAttributes.REGULAR_ATTRIBUTES)
            append("  ${value.file}:${value.line}", SimpleTextAttributes.GRAYED_ATTRIBUTES)
            append("  — ${value.reason}", SimpleTextAttributes.GRAYED_ITALIC_ATTRIBUTES)
        }
    }

    companion object {
        fun labelOf(v: Verdict): String = when (v) {
            Verdict.DELETION_CANDIDATE -> "DELETE?"
            Verdict.NEEDS_A_TEST -> "TEST"
            Verdict.UNDETERMINED -> "UNKNOWN"
        }

        /**
         * The one-line summary, separated so it is assertable without Swing.
         *
         * States each bucket, including undetermined — a summary that counted
         * only the two confident buckets would misrepresent how much the
         * analysis actually knows.
         */
        fun summarize(findings: List<UncoveredMethod>): String {
            if (findings.isEmpty()) return " Nothing uncovered."
            val delete = findings.count { it.verdict == Verdict.DELETION_CANDIDATE }
            val test = findings.count { it.verdict == Verdict.NEEDS_A_TEST }
            val unknown = findings.count { it.verdict == Verdict.UNDETERMINED }
            val parts = mutableListOf("$test need a test", "$delete unreachable")
            if (unknown > 0) parts += "$unknown undetermined"
            return " ${findings.size} uncovered: ${parts.joinToString(", ")}"
        }
    }
}
