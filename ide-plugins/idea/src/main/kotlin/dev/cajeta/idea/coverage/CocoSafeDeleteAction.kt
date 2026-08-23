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

    /**
     * The WHOLE member declaration at the finding's line, modifiers included.
     *
     * The grammar puts modifiers OUTSIDE the member:
     *
     * ```antlr
     * classBodyDeclaration : ... | modifier* memberDeclaration ;
     * memberDeclaration    : methodDeclaration | ... ;
     * ```
     *
     * and [CajetaNamedElement] maps `methodDeclaration`, which begins at the
     * return type. Handing that to Safe Delete removed the method and left its
     * modifiers behind:
     *
     * ```cajeta
     * /** The pre-2.0 tier ladder. */
     * public static            // <- all that remained; the file no longer parses
     * ```
     *
     * So the deletion target is the enclosing `classBodyDeclaration`. Nothing
     * downstream can recover from getting this wrong: Safe Delete's usage search
     * validates WHETHER to delete, never WHAT — an element that is half a
     * declaration passes every conflict check and still corrupts the file.
     */
    internal fun elementFor(
        project: com.intellij.openapi.project.Project,
        finding: UncoveredMethod,
    ): PsiElement? {
        val vf = CocoNavigation.resolve(project, finding.file) ?: return null
        val psi = PsiManager.getInstance(project).findFile(vf) ?: return null
        return elementAt(psi, finding.line)
    }

    /**
     * The member declaration on 1-based [line] of [psi].
     *
     * Split out from [elementFor] so it can be tested without a file on disk:
     * locating the FILE and locating the ELEMENT WITHIN IT are separate
     * problems, and the corruption was entirely in the second. Bundling them
     * meant the part that ate source could only be exercised through the part
     * that needs a real filesystem — which is why it never was.
     */
    internal fun elementAt(psi: com.intellij.psi.PsiFile, line: Int): PsiElement? {
        val document = com.intellij.psi.PsiDocumentManager.getInstance(psi.project)
            .getDocument(psi) ?: return null
        val zeroBased = (line - 1).coerceIn(0, (document.lineCount - 1).coerceAtLeast(0))
        // The line's first NON-WHITESPACE character, not its start. A line
        // starts with indentation, `findElementAt` on that returns the
        // whitespace token, and the walk upward from whitespace can surface the
        // enclosing CLASS instead of the method sitting on the line — which
        // would offer to delete the entire type.
        val text = document.charsSequence
        var offset = document.getLineStartOffset(zeroBased)
        val end = document.getLineEndOffset(zeroBased)
        while (offset < end && text[offset].isWhitespace()) offset++
        if (offset >= end) return null
        val at = psi.findElementAt(offset) ?: return null
        // Straight up to the enclosing `classBodyDeclaration` — NOT via
        // CajetaNamedElement. The anchor is the first token of the line, which
        // for `public static int64 f()` is `public`: part of `modifier*` in
        // classBodyDeclaration, and NOT part of methodDeclaration at all. So a
        // walk to the nearest named element steps over the method entirely and
        // surfaces the enclosing CLASS — offering to delete the whole type.
        // The declaration node is the target and the only correct one; ask for
        // it by name.
        return enclosingMember(at)
    }

    /**
     * Walk out to the `classBodyDeclaration` that owns [named].
     *
     * Returns null rather than guessing when there is none — a declaration that
     * is not inside a class body is not a shape this action understands, and
     * deleting the nearest thing to hand is how a refactoring eats source it was
     * never pointed at.
     */
    private fun enclosingMember(from: PsiElement): PsiElement? {
        var e: PsiElement? = from
        while (e != null && e !is com.intellij.psi.PsiFile) {
            if (dev.cajeta.idea.psi.ruleIndexOf(e) ==
                dev.cajeta.idea.parser.antlr.CajetaParser.RULE_classBodyDeclaration
            ) {
                return e
            }
            e = e.parent
        }
        return null
    }
}
