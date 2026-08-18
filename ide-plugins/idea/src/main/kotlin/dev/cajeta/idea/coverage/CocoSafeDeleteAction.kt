package dev.cajeta.idea.coverage

import com.intellij.openapi.actionSystem.AnActionEvent
import com.intellij.openapi.project.DumbAwareAction
import com.intellij.psi.PsiElement
import com.intellij.psi.PsiManager
import com.intellij.refactoring.safeDelete.SafeDeleteHandler
import dev.cajeta.idea.psi.CajetaNamedElement
import com.intellij.psi.util.PsiTreeUtil

/**
 * Safe Delete on a deletion candidate (spec §9.2, decided 2026-08-18).
 *
 * The platform's refactoring, not a delete of our own. That matters: Safe Delete
 * runs its **own** usage search and blocks on conflicts, so a false positive
 * from this static analysis meets an independent second opinion before any code
 * is removed. The classification is a suggestion; the refactoring is the check.
 *
 * The action is offered only for [Verdict.DELETION_CANDIDATE]. Offering it on a
 * needs-a-test row would invite deleting exactly the code someone should be
 * writing a test for.
 */
class CocoSafeDeleteAction(
    private val selection: () -> UncoveredMethod?,
) : DumbAwareAction("Safe Delete…") {

    override fun getActionUpdateThread() =
        com.intellij.openapi.actionSystem.ActionUpdateThread.BGT

    override fun update(e: AnActionEvent) {
        e.presentation.isEnabled = selection()?.verdict == Verdict.DELETION_CANDIDATE
    }

    override fun actionPerformed(e: AnActionEvent) {
        val project = e.project ?: return
        val finding = selection() ?: return
        if (finding.verdict != Verdict.DELETION_CANDIDATE) return
        val element = elementFor(project, finding) ?: return
        SafeDeleteHandler.invoke(project, arrayOf(element), true)
    }

    /** The declaration at the finding's line, or null when it cannot be found. */
    private fun elementFor(
        project: com.intellij.openapi.project.Project,
        finding: UncoveredMethod,
    ): PsiElement? {
        val vf = CocoNavigation.resolve(project, finding.file) ?: return null
        val psi = PsiManager.getInstance(project).findFile(vf) ?: return null
        val document = com.intellij.psi.PsiDocumentManager.getInstance(project)
            .getDocument(psi) ?: return null
        val line = (finding.line - 1).coerceIn(0, (document.lineCount - 1).coerceAtLeast(0))
        val at = psi.findElementAt(document.getLineStartOffset(line)) ?: return null
        return PsiTreeUtil.getParentOfType(at, CajetaNamedElement::class.java) ?: at
    }
}
