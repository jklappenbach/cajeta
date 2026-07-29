package dev.cajeta.idea.psi

import com.intellij.lang.ASTNode
import com.intellij.psi.PsiElement
import org.antlr.intellij.adaptor.psi.ANTLRPsiNode

/**
 * Base PSI node for every Cajeta rule element, overriding [getChildren] to
 * avoid the bundled antlr4-intellij-adaptor's `Trees.getChildren`.
 *
 * On IDE build 262+ (`com.intellij.openapi.command.WriteCommandAction` became
 * `final`) the adaptor's `Trees` class fails to link: its nested `Trees$1`
 * extends the now-final `WriteCommandAction`, so verifying `Trees` throws
 * `cannot inherit from final class`. `ANTLRPsiNode.getChildren` delegates to
 * `Trees.getChildren`, so every `.children` call — name resolution, Ctrl-mouse
 * navigation, structure view — blew up. The adaptor is stuck at 0.1 on Maven
 * Central (no newer release), so we bypass it: walk the AST children directly,
 * which is exactly what `Trees.getChildren` did (map `node.getChildren(null)`
 * to their PSI).
 */
open class CajetaPsiNode(node: ASTNode) : ANTLRPsiNode(node) {
    override fun getChildren(): Array<PsiElement> {
        val astChildren = node.getChildren(null)
        if (astChildren.isEmpty()) return PsiElement.EMPTY_ARRAY
        return Array(astChildren.size) { astChildren[it].psi }
    }
}
