package dev.cajeta.idea.usages

import com.intellij.openapi.application.ReadAction
import com.intellij.openapi.project.Project
import com.intellij.psi.PsiDocumentManager
import com.intellij.psi.PsiElement
import com.intellij.psi.PsiFile
import com.intellij.psi.PsiManager
import com.intellij.psi.PsiReference
import com.intellij.psi.search.FilenameIndex
import com.intellij.psi.search.GlobalSearchScope
import com.intellij.psi.search.searches.ReferencesSearch
import com.intellij.psi.util.PsiTreeUtil
import com.intellij.util.Processor
import com.intellij.util.QueryExecutor
import dev.cajeta.idea.psi.CajetaNamedElement
import dev.cajeta.idea.psi.CajetaTypeDeclaration
import dev.cajeta.idea.psi.packageOf
import dev.cajeta.idea.xref.XrefQuery

/**
 * Find Usages, answered from the index rather than by searching text
 * (ide-features spec §2.0.4).
 *
 * The platform's default would scan the word index for the identifier's text
 * and then ask each occurrence to resolve. That finds usages only where a
 * PSI reference happens to exist, spends the work on every same-named
 * identifier in the project, and cannot see into dependency or stdlib source
 * that is indexed but not open. The compiler already resolved every
 * reference and exported it under `uses:<fqn>`, so this returns exactly that
 * set: no text matching, no same-name false positives, and stdlib usages
 * appear like any others (§2.1.4).
 */
class CajetaUsagesSearch : QueryExecutor<PsiReference, ReferencesSearch.SearchParameters> {

    override fun execute(
        queryParameters: ReferencesSearch.SearchParameters,
        consumer: Processor<in PsiReference>,
    ): Boolean {
        val target = ReadAction.compute<PsiElement, RuntimeException> {
            queryParameters.elementToSearch
        }
        if (target !is CajetaNamedElement) return true
        val project = target.project
        val fqn = ReadAction.compute<String?, RuntimeException> { fqnOf(target) } ?: return true

        val sites = CajetaUsageRecords.parseAll(
            ReadAction.compute<List<dev.cajeta.idea.debugger.Json.Obj>, RuntimeException> {
                XrefQuery.usagesOf(project, fqn)
            })
        for (site in sites) {
            val ref = ReadAction.compute<PsiReference?, RuntimeException> {
                referenceAt(project, site)
            } ?: continue
            // Stop as soon as the consumer says it has enough — Show Usages
            // asks for a handful before the full search.
            if (!consumer.process(ref)) return false
        }
        return true
    }

    companion object {

        /**
         * The index key for a declaration: `package.Outer.Inner` for a type.
         *
         * Types only, deliberately. Methods and fields are keyed by overload
         * key in the export, and guessing that format here would silently
         * return nothing (or worse, another member's usages) rather than
         * failing visibly. They join when the key is confirmed.
         */
        fun fqnOf(element: PsiElement): String? {
            if (element !is CajetaNamedElement) return null
            if (element !is CajetaTypeDeclaration) return null
            val name = element.name ?: return null
            val enclosing = ArrayList<String>()
            var parent = PsiTreeUtil.getParentOfType(
                element, CajetaTypeDeclaration::class.java, true)
            while (parent != null) {
                parent.name?.let { enclosing.add(0, it) }
                parent = PsiTreeUtil.getParentOfType(
                    parent, CajetaTypeDeclaration::class.java, true)
            }
            val pkg = packageOf(element.containingFile)?.ifBlank { null }
            return (listOfNotNull(pkg) + enclosing + name).joinToString(".")
        }

        /** The PSI reference sitting at an exported use site, or null when the
         *  file has moved on since the index was written. */
        private fun referenceAt(
            project: Project,
            site: CajetaUsageRecords.UseSite,
        ): PsiReference? {
            val psi = psiFileFor(project, site.file) ?: return null
            val doc = PsiDocumentManager.getInstance(project).getDocument(psi) ?: return null
            if (site.line > doc.lineCount) return null
            val offset = doc.getLineStartOffset(site.line - 1) + site.col
            if (offset >= doc.textLength) return null
            val at = psi.findElementAt(offset) ?: return null
            return at.reference ?: at.parent?.reference
        }

        /** Same discipline as the reference adapter: a relative path can match
         *  several real files (a stdlib mount per compiler build, an archive
         *  extracted twice), so take the first that actually resolves. */
        private fun psiFileFor(project: Project, relPath: String): PsiFile? {
            val base = relPath.substringAfterLast('/')
            if (base.isBlank()) return null
            val scope = GlobalSearchScope.allScope(project)
            return FilenameIndex.getVirtualFilesByName(base, scope)
                .firstOrNull { it.path.endsWith("/$relPath") || it.path == relPath }
                ?.let { PsiManager.getInstance(project).findFile(it) }
        }
    }
}
