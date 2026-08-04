package dev.cajeta.idea.usages

import com.intellij.lang.cacheBuilder.WordsScanner
import com.intellij.lang.findUsages.FindUsagesProvider
import com.intellij.psi.PsiElement
import dev.cajeta.idea.psi.CajetaNamedElement
import dev.cajeta.idea.psi.CajetaTypeDeclaration

/**
 * Makes Find Usages available on a Cajeta declaration (ide-features §2.0.4).
 *
 * Registering this is what turns on the platform's whole usage machinery: the
 * right-click action, Ctrl+Click on a declaration showing the usages popup,
 * and the docked Find tool window with its file tree and source preview. None
 * of that is built here — the platform owns the presentation, this only says
 * what can be searched and how to name it (§1.4.1: register an extension
 * point, never build a panel).
 */
class CajetaFindUsagesProvider : FindUsagesProvider {

    /**
     * No words scanner. The platform uses one to build a text index for
     * "find by text" fallbacks; our search comes from the compiler's resolved
     * reference export ([CajetaUsagesSearch]), so a text index would add a
     * second, weaker answer to the same question. Null is the supported way to
     * say "this language searches by other means".
     */
    override fun getWordsScanner(): WordsScanner? = null

    override fun canFindUsagesFor(psiElement: PsiElement): Boolean =
        psiElement is CajetaNamedElement && psiElement.name != null

    override fun getHelpId(psiElement: PsiElement): String? = null

    override fun getType(element: PsiElement): String = when (element) {
        is CajetaTypeDeclaration -> "type"
        is CajetaNamedElement -> "declaration"
        else -> ""
    }

    /** What the Find window shows as the search subject — the qualified name,
     *  so two same-named types in different packages read apart (§2.1.6). */
    override fun getDescriptiveName(element: PsiElement): String =
        CajetaUsagesSearch.fqnOf(element)
            ?: (element as? CajetaNamedElement)?.name
            ?: ""

    override fun getNodeText(element: PsiElement, useFullName: Boolean): String =
        if (useFullName) getDescriptiveName(element)
        else (element as? CajetaNamedElement)?.name ?: ""
}
