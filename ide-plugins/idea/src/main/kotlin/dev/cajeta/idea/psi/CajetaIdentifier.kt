package dev.cajeta.idea.psi

import com.intellij.lang.ASTNode
import com.intellij.psi.PsiReference
import dev.cajeta.idea.xref.CajetaXrefReference
import org.antlr.intellij.adaptor.psi.ANTLRPsiNode

/**
 * A typed `identifier` rule node (ide-symbol-index Unit 7). Carries the xref
 * reference adapter directly — [ANTLRPsiNode] does not consult the reference-
 * provider registry, so contributing references externally would silently
 * never fire; owning getReference() here is deterministic (documented
 * deviation from plan 7.2.1's mechanism naming, same architecture).
 */
class CajetaIdentifier(node: ASTNode) : CajetaPsiNode(node) {

    /** A declaration's own name is not a reference to itself. */
    private fun isDeclarationName(): Boolean =
        (parent as? CajetaNamedElement)?.nameIdentifier === this

    override fun getReference(): PsiReference? =
        if (isDeclarationName()) null else CajetaXrefReference(this)
}
