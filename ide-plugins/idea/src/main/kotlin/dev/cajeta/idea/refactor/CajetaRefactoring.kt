package dev.cajeta.idea.refactor

import com.intellij.lang.refactoring.RefactoringSupportProvider
import com.intellij.openapi.project.Project
import com.intellij.psi.PsiElement
import com.intellij.psi.PsiReference
import com.intellij.psi.search.SearchScope
import com.intellij.refactoring.rename.RenamePsiElementProcessor
import com.intellij.refactoring.safeDelete.NonCodeUsageSearchInfo
import com.intellij.refactoring.safeDelete.SafeDeleteProcessorDelegate
import com.intellij.usageView.UsageInfo
import com.intellij.util.IncorrectOperationException
import dev.cajeta.idea.psi.CajetaNamedElement
import dev.cajeta.idea.usages.CajetaUsageRecords
import dev.cajeta.idea.usages.CajetaUsagesSearch
import dev.cajeta.idea.xref.CajetaXrefFreshness
import dev.cajeta.idea.xref.XrefQuery

/** Enables the platform's rename/safe-delete on Cajeta declarations. */
class CajetaRefactoringSupportProvider : RefactoringSupportProvider() {
    override fun isMemberInplaceRenameAvailable(element: PsiElement, context: PsiElement?): Boolean =
        element is CajetaNamedElement
    override fun isAvailable(element: PsiElement): Boolean = element is CajetaNamedElement
    override fun isSafeDeleteAvailable(element: PsiElement): Boolean = element is CajetaNamedElement
}

/**
 * Rename over the compiler's resolved reference set.
 *
 * The rewrite is the platform's; what matters here is WHICH references it is
 * handed, and the refusal. A partial reference set does not fail loudly — it
 * renames the declaration, misses a use site, and leaves code that no longer
 * compiles for a reason the developer did not cause. So when the index is not
 * FRESH this refuses outright rather than rewriting on a stale answer.
 */
class CajetaRenameProcessor : RenamePsiElementProcessor() {

    override fun canProcessElement(element: PsiElement): Boolean =
        element is CajetaNamedElement && element.name != null

    override fun findReferences(
        element: PsiElement,
        searchScope: SearchScope,
        searchInCommentsAndStrings: Boolean,
    ): Collection<PsiReference> {
        requireFreshIndex(element.project)
        return super.findReferences(element, searchScope, searchInCommentsAndStrings)
    }

    companion object {
        /**
         * Refuse unless the index is FRESH. `safeForRefactoring()` exists for
         * exactly this: a rename is the one operation where "probably right"
         * is not good enough, because the damage is silent.
         */
        fun requireFreshIndex(project: Project) {
            val fresh = try {
                project.getService(CajetaXrefFreshness::class.java)?.safeForRefactoring() ?: false
            } catch (_: Throwable) {
                false
            }
            if (!fresh) throw IncorrectOperationException(
                "The Cajeta index is not up to date, so the full set of usages " +
                    "cannot be guaranteed. Run Tools > Cajeta > Rebuild Cajeta " +
                    "Index and try again.")
        }
    }
}

/**
 * Safe delete: refuse while usages remain (spec 2.0.3).
 *
 * Every usage the compiler resolved is reported as a conflict, so the platform
 * shows them and asks — rather than deleting a declaration that half the
 * project still names.
 */
class CajetaSafeDeleteProcessor : SafeDeleteProcessorDelegate {

    override fun handlesElement(element: PsiElement): Boolean =
        element is CajetaNamedElement && element.name != null

    override fun findUsages(
        element: PsiElement,
        allElementsToDelete: Array<out PsiElement>,
        result: MutableList<in UsageInfo>,
    ): NonCodeUsageSearchInfo {
        val project = element.project
        val fqn = CajetaUsagesSearch.fqnOf(element)
        if (fqn != null) {
            val records = XrefQuery.usagesOf(project, fqn) +
                CajetaUsagesSearch.overloadKeyFor(project, element)
                    ?.let { XrefQuery.callersOf(project, it) }.orEmpty()
            // Count them: the conflict text is the honest summary, and the
            // platform's own search fills in the navigable rows.
            CajetaUsageRecords.parseAll(records)
        }
        return NonCodeUsageSearchInfo({ false }, element)
    }

    override fun getElementsToSearch(
        element: PsiElement,
        allElementsToDelete: MutableCollection<out PsiElement>,
    ): Collection<PsiElement> = listOf(element)

    override fun getAdditionalElementsToDelete(
        element: PsiElement,
        allElementsToDelete: MutableCollection<out PsiElement>,
        askUser: Boolean,
    ): Collection<PsiElement>? = null

    override fun findConflicts(
        element: PsiElement,
        allElementsToDelete: Array<out PsiElement>,
    ): Collection<String>? {
        val project = element.project
        // Stale index: say so instead of implying the declaration is unused.
        val fresh = try {
            project.getService(CajetaXrefFreshness::class.java)?.safeForRefactoring() ?: false
        } catch (_: Throwable) { false }
        if (!fresh) return listOf(
            "The Cajeta index is not up to date — remaining usages cannot be listed.")
        val fqn = CajetaUsagesSearch.fqnOf(element) ?: return null
        val records = XrefQuery.usagesOf(project, fqn) +
            CajetaUsagesSearch.overloadKeyFor(project, element)
                ?.let { XrefQuery.callersOf(project, it) }.orEmpty()
        val sites = CajetaUsageRecords.parseAll(records)
        if (sites.isEmpty()) return null
        val byFile = CajetaUsageRecords.byFile(sites)
        return listOf("${sites.size} usage(s) in ${byFile.size} file(s) still refer to this.")
    }

    override fun preprocessUsages(project: Project, usages: Array<out UsageInfo>): Array<UsageInfo>? =
        @Suppress("UNCHECKED_CAST") (usages as Array<UsageInfo>)

    override fun prepareForDeletion(element: PsiElement) {}

    override fun isToSearchInComments(element: PsiElement?): Boolean = false
    override fun setToSearchInComments(element: PsiElement?, enabled: Boolean) {}
    override fun isToSearchForTextOccurrences(element: PsiElement?): Boolean = false
    override fun setToSearchForTextOccurrences(element: PsiElement?, enabled: Boolean) {}
}
