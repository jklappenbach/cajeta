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
import com.intellij.openapi.diagnostic.Logger
import com.intellij.openapi.roots.ui.util.CompositeAppearance
import com.intellij.ui.SimpleTextAttributes
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
        val resolved = names.mapNotNull { name ->
            CajetaHierarchyLookup.declarationOf(myProject, name)?.let {
                CajetaTypeNodeDescriptor(myProject, descriptor, it)
            }
        }
        // An empty tree has three different causes — no edges indexed, edges
        // whose types will not resolve, or a stale shard set — and they are
        // indistinguishable on screen.
        LOG.debug("hierarchy " + (if (upward) "up" else "down") + " of " + fqn +
                  ": " + edges.size + " edge(s), " + names.size + " name(s), " +
                  resolved.size + " resolved")
        return resolved.toTypedArray()
    }
}

/**
 * A node in the tree. The text is built here explicitly: the base descriptor
 * renders from a language's own presentation support, which Cajeta has none
 * of, so a node left to the default draws NOTHING — an empty tree that looks
 * like a failed search (Julian, live 2026-08-02).
 */
class CajetaTypeNodeDescriptor(
    project: Project,
    parent: HierarchyNodeDescriptor?,
    element: PsiElement,
    private val base: Boolean = parent == null,
) : HierarchyNodeDescriptor(project, parent, element, base) {

    override fun update(): Boolean {
        val changed = super.update()
        val element = psiElement
        val fqn = element?.let { CajetaUsagesSearch.fqnOf(it) } ?: ""
        val simple = fqn.substringAfterLast('.').ifBlank { "?" }
        val pkg = fqn.substringBeforeLast('.', "")
        val text = CompositeAppearance()
        // The searched-for type reads bold, the way Java's browser marks it.
        val main = if (base) SimpleTextAttributes.REGULAR_BOLD_ATTRIBUTES
                   else SimpleTextAttributes.REGULAR_ATTRIBUTES
        text.ending.addText(simple, main.toTextAttributes())
        if (pkg.isNotBlank() && pkg != fqn) {
            text.ending.addText("  ($pkg)",
                SimpleTextAttributes.GRAYED_ATTRIBUTES.toTextAttributes())
        }
        myHighlightedText = text
        myName = simple
        return changed || true
    }
}

private val LOG = Logger.getInstance(CajetaTypeHierarchyStructure::class.java)
