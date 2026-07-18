package dev.cajeta.idea.xref

import com.intellij.openapi.util.TextRange
import com.intellij.psi.PsiDocumentManager
import com.intellij.psi.PsiElement
import com.intellij.psi.PsiElementResolveResult
import com.intellij.psi.PsiFile
import com.intellij.psi.PsiManager
import com.intellij.psi.PsiPolyVariantReferenceBase
import com.intellij.psi.ResolveResult
import com.intellij.psi.search.FilenameIndex
import com.intellij.psi.search.GlobalSearchScope
import com.intellij.psi.util.PsiTreeUtil
import dev.cajeta.idea.debugger.Json
import dev.cajeta.idea.parser.antlr.CajetaParser
import dev.cajeta.idea.psi.CajetaIdentifier
import dev.cajeta.idea.psi.CajetaMemberDeclaration
import dev.cajeta.idea.psi.CajetaNamedElement
import dev.cajeta.idea.psi.CajetaOperatorDeclaration
import dev.cajeta.idea.psi.CajetaParameterDeclaration
import dev.cajeta.idea.psi.CajetaTypeDeclaration
import dev.cajeta.idea.psi.CajetaTypeParameterDeclaration
import dev.cajeta.idea.psi.CajetaVariableDeclaration
import dev.cajeta.idea.psi.directChildRule
import dev.cajeta.idea.psi.packageOf
import dev.cajeta.idea.psi.ruleIndexOf

/**
 * The reference ADAPTER (ide-symbol-index Unit 7, spec §4): looks its own
 * (file, line, col) up in the index, follows the compiler's answer to a
 * declaration record, and returns the PSI element there. No scope walking, no
 * import resolution, no overload selection here (§4.0.2). Ambiguity — several
 * declarations claiming the exported target — surfaces as the candidate set
 * (§4.0.3). No index entry → the one local fallback (§4.3), else unresolved,
 * never a throw (§4.0.5). Soft: an unresolved name is the index's silence,
 * not a proven error, so no red squiggle.
 */
class CajetaXrefReference(element: CajetaIdentifier) :
    PsiPolyVariantReferenceBase<CajetaIdentifier>(
        element, TextRange(0, element.textLength), /* soft = */ true) {

    override fun multiResolve(incompleteCode: Boolean): Array<ResolveResult> {
        val project = element.project
        val file = element.containingFile ?: return ResolveResult.EMPTY_ARRAY
        val doc = PsiDocumentManager.getInstance(project).getDocument(file)
        if (doc != null) {
            val off = element.textRange.startOffset
            val line = doc.getLineNumber(off) + 1
            val col = off - doc.getLineStartOffset(line - 1)
            val rel = xrefRelPath(file)
            if (rel != null) {
                val use = XrefQuery.usesIn(project, rel).firstOrNull {
                    intOf(it, "line") == line && intOf(it, "col") == col
                }
                if (use != null) {
                    val decls = when {
                        use.opt("target") is Json.Str ->
                            XrefQuery.declarationsOf(
                                project, (use.opt("target") as Json.Str).value)
                        use.opt("callee") is Json.Str ->
                            XrefQuery.declarationsForOverloadKey(
                                project, (use.opt("callee") as Json.Str).value)
                        else -> emptyList()
                    }
                    val targets = decls.mapNotNull { locate(project, it) }
                    if (targets.isNotEmpty()) {
                        return targets.map(::PsiElementResolveResult).toTypedArray()
                    }
                }
            }
        }
        // The one local fallback (§4.3): locals, parameters, type parameters —
        // bound within the subtree they are written in; the IDE cannot be
        // wrong about them, and they must survive an un-relinted buffer.
        val local = localFallback(element)
        return if (local != null) arrayOf(PsiElementResolveResult(local))
               else ResolveResult.EMPTY_ARRAY
    }

    override fun getVariants(): Array<Any> = emptyArray()

    companion object {

        private fun intOf(o: Json.Obj, field: String): Int? =
            (o.opt(field) as? Json.Num)?.value?.toInt()

        private fun strOf(o: Json.Obj, field: String): String? =
            (o.opt(field) as? Json.Str)?.value

        /**
         * The file's export identity: root-relative path derived from the
         * package — exactly how the compiler derives it (FQNs are
         * path-derived; the `package` declaration is validated against the
         * path, never adopted).
         */
        fun xrefRelPath(file: PsiFile): String? {
            val pkg = packageOf(file) ?: return file.name
            return pkg.replace('.', '/') + '/' + file.name
        }

        /**
         * A declaration record's (file, line, col) → the named element there —
         * VALIDATED (Unit 9, 9.2.1): the export's offset is followed only if
         * what sits there still matches the recorded declaration's name. A
         * moved declaration makes the offset point at whatever occupies it
         * now; following it blindly would be a confident wrong jump — the
         * failure mode this design exists to prevent. Stale → DISCARDED.
         */
        private fun locate(project: com.intellij.openapi.project.Project,
                           decl: Json.Obj): PsiElement? {
            val rel = strOf(decl, "file") ?: return null
            val line = intOf(decl, "line") ?: return null
            val col = intOf(decl, "col") ?: return null
            val base = rel.substringAfterLast('/')
            val scope = GlobalSearchScope.allScope(project)
            val vFile = FilenameIndex.getVirtualFilesByName(base, scope)
                .firstOrNull { it.path.endsWith("/$rel") || it.path == rel }
                ?: return null
            val psi = PsiManager.getInstance(project).findFile(vFile) ?: return null
            val doc = PsiDocumentManager.getInstance(project).getDocument(psi)
                ?: return null
            if (line > doc.lineCount) return null
            val offset = doc.getLineStartOffset(line - 1) + col
            if (offset > doc.textLength) return null
            val at = psi.findElementAt(offset) ?: return null
            val named = PsiTreeUtil.getParentOfType(
                at, CajetaNamedElement::class.java, false) ?: return null
            val expected = strOf(decl, "fqn")?.substringAfterLast('.')
            if (expected != null && named.name != expected) return null
            return named
        }

        /** §4.3 — nothing here can be got wrong: each name binds inside the
         *  PSI subtree it is written in. Fields deliberately never resolve
         *  from PSI alone (class scope is the compiler's business). */
        private fun localFallback(id: CajetaIdentifier): PsiElement? {
            val name = id.text ?: return null
            val member = PsiTreeUtil.getParentOfType(
                id, CajetaMemberDeclaration::class.java,
                CajetaOperatorDeclaration::class.java)

            if (member != null) {
                // Locals declared before the use, nearest last (shadowing).
                val locals = PsiTreeUtil
                    .findChildrenOfType(member, CajetaVariableDeclaration::class.java)
                    .filter {
                        ruleIndexOf(it as PsiElement) ==
                            CajetaParser.RULE_localVariableDeclaration
                    }
                val candidates = locals.flatMap { decl ->
                    val perName = decl.declarators.filter { it.name == name }
                    // `var x = ...` names a bare identifier on the declaration.
                    if (perName.isEmpty() && decl.name == name) listOf(decl)
                    else perName
                }.filter { it.textOffset < id.textOffset }
                candidates.maxByOrNull { it.textOffset }?.let { return it }

                // Parameters — under the member's own formalParameters only.
                directChildRule(member as PsiElement,
                        CajetaParser.RULE_formalParameters)?.let { params ->
                    PsiTreeUtil.findChildrenOfType(
                            params, CajetaParameterDeclaration::class.java)
                        .firstOrNull { it.name == name }?.let { return it }
                }
            }

            // Type parameters: the declaring member's or enclosing types' OWN
            // typeParameters child — never one plucked from a nested subtree.
            var scope: PsiElement? = id
            while (scope != null) {
                if (scope is CajetaMemberDeclaration || scope is CajetaTypeDeclaration) {
                    directChildRule(scope, CajetaParser.RULE_typeParameters)
                        ?.children
                        ?.filterIsInstance<CajetaTypeParameterDeclaration>()
                        ?.firstOrNull { it.name == name }
                        ?.let { return it }
                }
                scope = scope.parent
            }
            return null
        }
    }
}
