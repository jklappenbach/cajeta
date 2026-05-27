package dev.cajeta.idea.editor

import com.intellij.ide.structureView.StructureViewBuilder
import com.intellij.ide.structureView.StructureViewModel
import com.intellij.ide.structureView.StructureViewModelBase
import com.intellij.ide.structureView.StructureViewTreeElement
import com.intellij.ide.structureView.TreeBasedStructureViewBuilder
import com.intellij.ide.util.treeView.smartTree.SortableTreeElement
import com.intellij.ide.util.treeView.smartTree.TreeElement
import com.intellij.lang.PsiStructureViewFactory
import com.intellij.navigation.ItemPresentation
import com.intellij.openapi.editor.Editor
import com.intellij.psi.PsiElement
import com.intellij.psi.PsiFile
import com.intellij.psi.tree.IElementType
import dev.cajeta.idea.CajetaIcons
import dev.cajeta.idea.CajetaLanguage
import dev.cajeta.idea.parser.CajetaPsiFile
import org.antlr.intellij.adaptor.lexer.RuleIElementType
import org.antlr.intellij.adaptor.lexer.PSIElementTypeFactory
import javax.swing.Icon

class CajetaStructureViewFactory : PsiStructureViewFactory {
    override fun getStructureViewBuilder(psiFile: PsiFile): StructureViewBuilder =
        object : TreeBasedStructureViewBuilder() {
            override fun createStructureViewModel(editor: Editor?): StructureViewModel =
                CajetaStructureViewModel(psiFile as CajetaPsiFile)
        }
}

private class CajetaStructureViewModel(file: CajetaPsiFile) :
    StructureViewModelBase(file, CajetaStructureElement(file)),
    StructureViewModel.ElementInfoProvider {

    init {
        withSuitableClasses(CajetaPsiFile::class.java)
    }

    override fun isAlwaysShowsPlus(element: StructureViewTreeElement): Boolean = false
    override fun isAlwaysLeaf(element: StructureViewTreeElement): Boolean = false
}

/**
 * Wraps an ANTLR PSI node into a structure-view tree element. The
 * children of a node are filtered down to declaration-shaped rule
 * nodes — package, class, interface, structure, enum, method,
 * constructor, field, enum-constant. Everything else is skipped.
 */
private class CajetaStructureElement(private val element: PsiElement) :
    StructureViewTreeElement, SortableTreeElement {

    override fun getValue(): Any = element

    override fun navigate(requestFocus: Boolean) {
        (element as? com.intellij.pom.Navigatable)?.navigate(requestFocus)
    }

    override fun canNavigate(): Boolean =
        (element as? com.intellij.pom.Navigatable)?.canNavigate() ?: false

    override fun canNavigateToSource(): Boolean = canNavigate()

    override fun getAlphaSortKey(): String = presentation.presentableText ?: ""

    override fun getPresentation(): ItemPresentation = object : ItemPresentation {
        override fun getPresentableText(): String = textFor(element)
        override fun getLocationString(): String? = null
        override fun getIcon(unused: Boolean): Icon? = iconFor(element)
    }

    override fun getChildren(): Array<TreeElement> {
        return element.children
            .filter { isInteresting(it) }
            .map { CajetaStructureElement(it) }
            .toTypedArray()
    }

    companion object {
        private fun ruleType(element: PsiElement): String? {
            val type: IElementType = element.node?.elementType ?: return null
            return (type as? RuleIElementType)?.toString()
        }

        private fun isInteresting(element: PsiElement): Boolean =
            ruleType(element) in INTERESTING_RULES

        private fun textFor(element: PsiElement): String {
            val type = ruleType(element)
            val rawText = element.text
            // Show the first identifier we find in the declaration as
            // the label. Falls back to the rule kind if there's no
            // identifier (eg. anonymous constructs).
            val firstIdentifier = firstIdentifierIn(element)
            return when (type) {
                null -> rawText.lineSequence().firstOrNull() ?: rawText
                "compilationUnit" -> element.containingFile?.name ?: "compilationUnit"
                "packageDeclaration" -> "package " + (firstIdentifier ?: "")
                else -> firstIdentifier ?: (type + " (anonymous)")
            }
        }

        private fun firstIdentifierIn(element: PsiElement): String? {
            // BFS for the first leaf whose ANTLR token name is IDENTIFIER.
            val queue = ArrayDeque<PsiElement>()
            queue += element
            while (queue.isNotEmpty()) {
                val node = queue.removeFirst()
                val nodeType = node.node?.elementType
                if (nodeType != null && nodeType.toString() == "IDENTIFIER") {
                    return node.text
                }
                queue += node.children
            }
            return null
        }

        private fun iconFor(element: PsiElement): Icon? {
            // Single icon for now — refine when we add per-kind icons.
            return CajetaIcons.FILE.takeIf { ruleType(element) != null }
        }

        private val INTERESTING_RULES: Set<String> = setOf(
            "compilationUnit",
            "packageDeclaration",
            "typeDeclaration",
            "classDeclaration",
            "interfaceDeclaration",
            "enumDeclaration",
            "memberDeclaration",
            "methodDeclaration",
            "constructorDeclaration",
            "fieldDeclaration",
            "interfaceMethodDeclaration",
            "enumConstant",
        )

        init {
            // Touch the registry once so RuleIElementType.toString() is
            // populated when the structure view first asks.
            PSIElementTypeFactory.getRuleIElementTypes(CajetaLanguage)
        }
    }
}
