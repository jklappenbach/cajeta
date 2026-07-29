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
import dev.cajeta.idea.CajetaIcons
import dev.cajeta.idea.parser.CajetaPsiFile
import dev.cajeta.idea.parser.antlr.CajetaParser
import dev.cajeta.idea.psi.CajetaEnumConstantDeclaration
import dev.cajeta.idea.psi.CajetaMemberDeclaration
import dev.cajeta.idea.psi.CajetaNamedElement
import dev.cajeta.idea.psi.CajetaOperatorDeclaration
import dev.cajeta.idea.psi.CajetaTypeDeclaration
import dev.cajeta.idea.psi.CajetaVariableDeclaration
import dev.cajeta.idea.psi.directChildRule
import dev.cajeta.idea.psi.dottedName
import dev.cajeta.idea.psi.ruleIndexOf
import javax.swing.Icon

/**
 * Structure view over the typed named elements (ide-symbol-index Unit 5,
 * plan 5.2.4). The previous implementation string-matched ANTLR rule names on
 * DIRECT children and BFS-ed for the first IDENTIFIER leaf — which stopped at
 * `classBody` (not in its interesting set), so members never actually appeared,
 * and a declaration's label could be any identifier that happened to lex first.
 * Named elements carry their own names; structure children are found by
 * descending through untyped wrapper rules until a structure-worthy element
 * appears, then letting that element own its subtree.
 */
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

    override fun getChildren(): Array<TreeElement> =
        structureChildrenOf(element)
            .map { CajetaStructureElement(it) }
            .toTypedArray<TreeElement>()

    companion object {

        private fun textFor(element: PsiElement): String = when {
            element is PsiFile -> element.name
            ruleIndexOf(element) == CajetaParser.RULE_packageDeclaration -> {
                val qn = directChildRule(element, CajetaParser.RULE_qualifiedName)
                "package " + (qn?.let { dottedName(it) } ?: "")
            }
            element is CajetaNamedElement ->
                element.name ?: "(anonymous)"
            else -> element.text.lineSequence().firstOrNull() ?: ""
        }

        private fun iconFor(element: PsiElement): Icon? =
            CajetaIcons.FILE.takeIf { element !is PsiFile }

        /**
         * Structure-worthy elements beneath `root`, in source order: descend
         * through untyped wrappers (typeDeclaration, classBody,
         * classBodyDeclaration, ...), stop at each structure element. A
         * multi-name field contributes one entry PER declared name.
         */
        private fun structureChildrenOf(root: PsiElement): List<PsiElement> {
            val out = mutableListOf<PsiElement>()
            fun descend(e: PsiElement) {
                for (child in e.children) {
                    when {
                        child is CajetaTypeDeclaration ||
                        child is CajetaMemberDeclaration ||
                        child is CajetaOperatorDeclaration ||
                        child is CajetaEnumConstantDeclaration -> out += child

                        child is CajetaVariableDeclaration &&
                        ruleIndexOf(child as PsiElement) ==
                            CajetaParser.RULE_fieldDeclaration ->
                            out += child.declarators

                        root is PsiFile && ruleIndexOf(child) ==
                            CajetaParser.RULE_packageDeclaration -> out += child

                        // Locals and their scopes: a method's subtree is its
                        // own; do not descend past another named element.
                        child is CajetaNamedElement -> { /* stop */ }

                        else -> descend(child)
                    }
                }
            }
            descend(root)
            return out
        }
    }
}
