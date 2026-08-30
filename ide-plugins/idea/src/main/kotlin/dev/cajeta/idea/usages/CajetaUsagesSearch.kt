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
                // A type is referenced; a method is CALLED. The export keys the
                // two relations separately, so ask both and let whichever
                // applies answer — a member gets its call sites, a type its
                // references, and neither needs to know which it is.
                XrefQuery.usagesOf(project, fqn) +
                    overloadKeyFor(project, target)
                        ?.let { key ->
                            // A VIRTUAL call names the STATIC receiver's method,
                            // never the override that will actually run, so an
                            // override has no callers of its own and Find Usages
                            // on it reported nothing — while the call plainly
                            // existed. Measured on the tour 2026-08-30:
                            // `Tour.cajeta:137` calls `DemoClass::execute`
                            // through `demos.forEach((d) -> d.execute())`, and
                            // Find Usages on any demo's `execute()` was empty.
                            //
                            // So ask for the callers of everything this method
                            // overrides as well. Those sites reach it, and are
                            // the honest answer to "what calls this".
                            (listOf(key) + overriddenChain(key) { k ->
                                XrefQuery.overriddenBy(project, k).mapNotNull {
                                    (it.entries["overrides"]
                                        as? dev.cajeta.idea.debugger.Json.Str)?.value
                                }
                            }).flatMap { XrefQuery.callersOf(project, it) }
                        }
                        .orEmpty()
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
        /**
         * Every overload key [start] overrides, transitively and excluding
         * itself, in discovery order.
         *
         * Pure and injected so the WALK is testable without an index: the bug
         * this exists for was not in any query — both `overriddenBy` and
         * `callersOf` already worked — it was that nothing joined them.
         *
         * The visited set is a cycle guard. A well-formed export cannot
         * describe a method that transitively overrides itself, but this walks
         * machine-generated data during Find Usages, on the EDT's search
         * thread, and a malformed index must not hang the IDE. Depth is capped
         * for the same reason: an unbounded walk over a corrupt index is a
         * freeze, and a truncated answer is merely incomplete.
         */
        fun overriddenChain(
            start: String,
            maxDepth: Int = 32,
            overriddenBy: (String) -> List<String>,
        ): List<String> {
            val seen = LinkedHashSet<String>()
            var frontier = listOf(start)
            var depth = 0
            while (frontier.isNotEmpty() && depth < maxDepth) {
                val next = ArrayList<String>()
                for (key in frontier) {
                    for (parent in overriddenBy(key)) {
                        if (parent.isBlank() || parent == start) continue
                        if (seen.add(parent)) next.add(parent)
                    }
                }
                frontier = next
                depth++
            }
            return seen.toList()
        }

        /**
         * The overload key for a MEMBER, read from its own declaration record
         * rather than constructed.
         *
         * Constructing it would mean re-implementing the compiler's mangling
         * in Kotlin and keeping the two in step forever; a wrong guess returns
         * nothing, or another overload's usages, without ever failing visibly.
         * The declaration already carries the key, so the only real problem is
         * picking the RIGHT declaration when a name is overloaded — settled by
         * position, since two overloads cannot share a line.
         */
        fun overloadKeyFor(project: Project, element: PsiElement): String? {
            if (element !is CajetaNamedElement) return null
            if (element is CajetaTypeDeclaration) return null
            val dotted = dottedNameOf(element) ?: return null
            val file = element.containingFile ?: return null
            val doc = PsiDocumentManager.getInstance(project).getDocument(file)
            val line = doc?.getLineNumber(element.textOffset)?.plus(1)
            val records = XrefQuery.declarationsOf(project, dotted)
            if (records.isEmpty()) return null
            val exact = records.firstOrNull { r ->
                (r.entries["line"] as? dev.cajeta.idea.debugger.Json.Num)
                    ?.value?.toInt() == line
            }
            // With no position match, a SINGLE candidate is unambiguous; more
            // than one means overloads we cannot tell apart, and answering
            // with the wrong one is worse than answering with none (§2.1.3).
            val chosen = exact ?: records.singleOrNull() ?: return null
            return (chosen.entries["overloadKey"] as? dev.cajeta.idea.debugger.Json.Str)
                ?.value?.ifBlank { null }
        }

        /** `package.Outer.member` — the name a declaration record is filed
         *  under, for a type or a member alike. */
        private fun dottedNameOf(element: PsiElement): String? {
            val named = element as? CajetaNamedElement ?: return null
            val name = named.name ?: return null
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

        fun fqnOf(element: PsiElement): String? {
            if (element !is CajetaNamedElement) return null
            if (element !is CajetaTypeDeclaration) return dottedNameOf(element)
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
