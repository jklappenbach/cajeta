package dev.cajeta.idea.hierarchy

import com.intellij.ide.hierarchy.HierarchyBrowser
import com.intellij.ide.hierarchy.HierarchyNodeDescriptor
import com.intellij.ide.hierarchy.HierarchyProvider
import com.intellij.ide.hierarchy.HierarchyTreeStructure
import com.intellij.ide.hierarchy.TypeHierarchyBrowserBase
import com.intellij.ide.util.treeView.NodeDescriptor
import com.intellij.openapi.actionSystem.DataContext
import com.intellij.openapi.actionSystem.CommonDataKeys
import com.intellij.openapi.project.Project
import com.intellij.psi.PsiElement
import com.intellij.psi.util.PsiTreeUtil
import dev.cajeta.idea.psi.CajetaTypeDeclaration
import dev.cajeta.idea.usages.CajetaUsagesSearch
import dev.cajeta.idea.xref.XrefQuery
import java.util.Comparator
import javax.swing.JTree

/**
 * Type Hierarchy (Ctrl+H) over the compiler's inheritance export
 * (ide-features Unit 3).
 *
 * Like find usages, the relation is already resolved and exported — both
 * `extends` and `implements` edges, with both endpoints — so no part of this
 * infers a supertype from names or imports. The three views the platform
 * expects (supertypes, subtypes, and both) are three walks over the same
 * edges.
 */
class CajetaTypeHierarchyProvider : HierarchyProvider {

    override fun getTarget(dataContext: DataContext): PsiElement? {
        val element = CommonDataKeys.PSI_ELEMENT.getData(dataContext)
            ?: CommonDataKeys.PSI_FILE.getData(dataContext)?.let { file ->
                val editor = CommonDataKeys.EDITOR.getData(dataContext) ?: return@let null
                file.findElementAt(editor.caretModel.offset)
            }
            ?: return null
        return PsiTreeUtil.getParentOfType(element, CajetaTypeDeclaration::class.java, false)
    }

    override fun createHierarchyBrowser(target: PsiElement): HierarchyBrowser =
        CajetaTypeHierarchyBrowser(target.project, target)

    override fun browserActivated(hierarchyBrowser: HierarchyBrowser) {
        (hierarchyBrowser as CajetaTypeHierarchyBrowser)
            .changeView(TypeHierarchyBrowserBase.getTypeHierarchyType())
    }
}

class CajetaTypeHierarchyBrowser(project: Project, element: PsiElement) :
    TypeHierarchyBrowserBase(project, element) {

    override fun isInterface(psiElement: PsiElement): Boolean {
        // The export says which edges are `implements`; a type's own
        // interface-ness is a declaration fact the PSI carries.
        val text = (psiElement as? CajetaTypeDeclaration)?.text ?: return false
        return Regex("\\binterface\\s").containsMatchIn(text.take(200))
    }

    override fun canBeDeleted(psiElement: PsiElement): Boolean = false

    override fun getQualifiedName(psiElement: PsiElement): String =
        CajetaUsagesSearch.fqnOf(psiElement) ?: ""

    override fun isApplicableElement(psiElement: PsiElement): Boolean =
        psiElement is CajetaTypeDeclaration

    override fun getElementFromDescriptor(descriptor: HierarchyNodeDescriptor): PsiElement? =
        descriptor.psiElement

    override fun createLegendPanel(): javax.swing.JPanel? = null

    override fun getComparator(): Comparator<NodeDescriptor<*>>? =
        Comparator { a, b -> a.toString().compareTo(b.toString()) }

    override fun createTrees(trees: MutableMap<in String, in JTree>) {
        for (type in listOf(getTypeHierarchyType(), getSupertypesHierarchyType(),
                            getSubtypesHierarchyType())) {
            trees[type] = createTree(false)
        }
    }

    override fun createHierarchyTreeStructure(
        type: String,
        psiElement: PsiElement,
    ): HierarchyTreeStructure? = when (type) {
        getSupertypesHierarchyType() ->
            CajetaTypeHierarchyStructure(myProject, psiElement, upward = true)
        getSubtypesHierarchyType(), getTypeHierarchyType() ->
            CajetaTypeHierarchyStructure(myProject, psiElement, upward = false)
        else -> null
    }
}

/**
 * One direction of the hierarchy, expanded lazily: children are computed only
 * when a node is opened, so a wide relation costs nothing until it is looked
 * at (3.1.6).
 */
class CajetaTypeHierarchyStructure(
    project: Project,
    element: PsiElement,
    private val upward: Boolean,
) : HierarchyTreeStructure(project, CajetaTypeNodeDescriptor(project, null, element)) {

    override fun buildChildren(descriptor: HierarchyNodeDescriptor): Array<Any> {
        val element = descriptor.psiElement ?: return emptyArray()
        val fqn = CajetaUsagesSearch.fqnOf(element) ?: return emptyArray()
        val edges = CajetaInheritance.parseAll(
            if (upward) XrefQuery.supertypesOf(myProject, fqn)
            else XrefQuery.subtypesOf(myProject, fqn))
        val names =
            if (upward) CajetaInheritance.parentsOf(edges).map { it.parent }
            else CajetaInheritance.childrenOf(edges).map { it.child }
        return names.mapNotNull { name ->
            CajetaHierarchyLookup.declarationOf(myProject, name)?.let {
                CajetaTypeNodeDescriptor(myProject, descriptor, it)
            }
        }.toTypedArray()
    }
}

class CajetaTypeNodeDescriptor(
    project: Project,
    parent: HierarchyNodeDescriptor?,
    element: PsiElement,
) : HierarchyNodeDescriptor(project, parent, element, parent == null)
