package dev.cajeta.idea.psi

import com.intellij.lang.ASTNode
import com.intellij.psi.PsiComment
import com.intellij.psi.PsiElement
import com.intellij.psi.PsiFile
import com.intellij.psi.PsiFileFactory
import com.intellij.psi.PsiNameIdentifierOwner
import com.intellij.psi.PsiWhiteSpace
import com.intellij.psi.util.PsiTreeUtil
import com.intellij.util.IncorrectOperationException
import dev.cajeta.idea.CajetaLanguage
import dev.cajeta.idea.parser.antlr.CajetaLexer
import dev.cajeta.idea.parser.antlr.CajetaParser
import org.antlr.intellij.adaptor.lexer.RuleIElementType
import org.antlr.intellij.adaptor.lexer.TokenIElementType
import org.antlr.intellij.adaptor.psi.ANTLRPsiNode

/**
 * ide-symbol-index Unit 5 (spec §3): named PSI elements. Pure syntax — the IDE
 * owns naming and nothing beyond it. Each declaring rule yields one of these
 * instead of a blanket [ANTLRPsiNode]; everything else stays untyped. Rename
 * targets these ([PsiNameIdentifierOwner]); the xref presentation layer (Unit 7)
 * maps export records onto them by FQN + offset (§3.0.4).
 */
internal fun ruleIndexOf(element: PsiElement?): Int =
    (element?.node?.elementType as? RuleIElementType)?.ruleIndex ?: -1

internal fun ruleIndexOf(node: ASTNode): Int =
    (node.elementType as? RuleIElementType)?.ruleIndex ?: -1

internal fun directChildRule(element: PsiElement, ruleIndex: Int): PsiElement? =
    element.children.firstOrNull { ruleIndexOf(it) == ruleIndex }

/** The dotted text of a `qualifiedName` rule node, whitespace-proof. */
internal fun dottedName(qualifiedName: PsiElement): String =
    qualifiedName.children
        .filter { ruleIndexOf(it) == CajetaParser.RULE_identifier }
        .joinToString(".") { it.text }

/** The file's declared package, or null. */
internal fun packageOf(file: PsiFile?): String? {
    // The tree is FILE → compilationUnit → packageDeclaration; tolerate the
    // package sitting at file level too (error-recovery trees).
    val scopes = listOfNotNull(
        file,
        file?.children?.firstOrNull {
            ruleIndexOf(it) == CajetaParser.RULE_compilationUnit
        },
    )
    val pkg = scopes.firstNotNullOfOrNull { scope ->
        scope.children.firstOrNull {
            ruleIndexOf(it) == CajetaParser.RULE_packageDeclaration
        }
    } ?: return null
    val qn = directChildRule(pkg, CajetaParser.RULE_qualifiedName) ?: return null
    return dottedName(qn)
}

abstract class CajetaNamedElement(node: ASTNode) : ANTLRPsiNode(node),
    PsiNameIdentifierOwner {

    abstract override fun getNameIdentifier(): PsiElement?

    override fun getName(): String? = nameIdentifier?.text

    /**
     * Replace the identifier and nothing else. The replacement is parsed out of
     * a throwaway fragment so it is a real `identifier` rule node, not a bare
     * leaf grafted where a composite belongs.
     */
    override fun setName(name: String): PsiElement {
        val id = nameIdentifier ?: throw IncorrectOperationException("unnamed element")
        if (ruleIndexOf(id) != CajetaParser.RULE_identifier) {
            throw IncorrectOperationException("not a renamable identifier")
        }
        id.replace(createIdentifier(this, name))
        return this
    }

    /**
     * The FQN the xref export keys on (§3.0.4), or null where the export has no
     * key for this element (locals, parameters, type parameters).
     */
    open val fqn: String? get() = null

    /** Caret lands on the name, not the modifiers. */
    override fun getTextOffset(): Int =
        nameIdentifier?.textRange?.startOffset ?: super.getTextOffset()

    protected fun ownerFqn(): String? =
        PsiTreeUtil.getParentOfType(this, CajetaTypeDeclaration::class.java)?.fqn

    companion object {
        private fun createIdentifier(context: PsiElement, name: String): PsiElement {
            val file = PsiFileFactory.getInstance(context.project)
                .createFileFromText("__rename__.cajeta", CajetaLanguage, "class $name {}")
            val decl = PsiTreeUtil.findChildOfType(file, CajetaTypeDeclaration::class.java)
            return decl?.nameIdentifier
                ?: throw IncorrectOperationException("'$name' is not a valid identifier")
        }
    }
}

/** class / interface / enum / record / view / annotation declarations. */
class CajetaTypeDeclaration(node: ASTNode) : CajetaNamedElement(node) {

    override fun getNameIdentifier(): PsiElement? =
        directChildRule(this, CajetaParser.RULE_identifier)

    val isAnnotation: Boolean
        get() = ruleIndexOf(node) == CajetaParser.RULE_annotationTypeDeclaration

    override val fqn: String?
        get() {
            val n = name ?: return null
            // The compiler canonicalizes EVERY annotation under the synthetic
            // "code" package (visitAnnotationTypeDeclaration) — the export says
            // code.<Name>, so this element must too, or the two can never meet.
            if (isAnnotation) return "code.$n"
            PsiTreeUtil.getParentOfType(this, CajetaTypeDeclaration::class.java)
                ?.let { outer -> return outer.fqn?.let { "$it.$n" } }
            val pkg = packageOf(containingFile)
            return if (pkg.isNullOrEmpty()) n else "$pkg.$n"
        }
}

/**
 * Members whose name is a direct `identifier` child: methods, constructors,
 * destructors, interface methods (`interfaceCommonBodyDeclaration` — the rule
 * that actually holds the identifier), and annotation elements
 * (`annotationMethodRest`).
 */
class CajetaMemberDeclaration(node: ASTNode) : CajetaNamedElement(node) {

    override fun getNameIdentifier(): PsiElement? =
        directChildRule(this, CajetaParser.RULE_identifier)

    override val fqn: String?
        get() {
            val n = name ?: return null
            return ownerFqn()?.let { "$it.$n" }
        }
}

/**
 * Operator overloads (§3.0.5). The "identifier" is the operator token after the
 * `operator` keyword; the display name is `operator<token>`. Not renamable —
 * the name IS the operator.
 */
class CajetaOperatorDeclaration(node: ASTNode) : CajetaNamedElement(node) {

    override fun getNameIdentifier(): PsiElement? {
        var seenKeyword = false
        var child: PsiElement? = firstChild
        while (child != null) {
            val tokenType =
                (child.node?.elementType as? TokenIElementType)?.antlrTokenType
            if (seenKeyword && child !is PsiWhiteSpace && child !is PsiComment) {
                return child
            }
            if (tokenType == CajetaLexer.OPERATOR) seenKeyword = true
            child = child.nextSibling
        }
        return null
    }

    override fun getName(): String? = nameIdentifier?.text?.let { "operator$it" }

    override fun setName(name: String): PsiElement =
        throw IncorrectOperationException("operator declarations are not renamable")

    override val fqn: String?
        get() {
            val n = name ?: return null
            return ownerFqn()?.let { "$it.$n" }
        }
}

/**
 * One declared NAME — a `variableDeclaratorId`. `int32 count, backup;` declares
 * two of these, each its own rename target; an owner whose nameIdentifier were
 * `count` would make Rename on `backup` silently target `count` — a wrong
 * answer, the failure mode this layer exists to avoid.
 */
class CajetaVariableName(node: ASTNode) : CajetaNamedElement(node) {

    override fun getNameIdentifier(): PsiElement? =
        directChildRule(this, CajetaParser.RULE_identifier)

    override val fqn: String?
        get() {
            // Only FIELD names have an export key (declaring class + name).
            val decl = PsiTreeUtil.getParentOfType(
                this, CajetaVariableDeclaration::class.java) ?: return null
            if (ruleIndexOf(decl as PsiElement) != CajetaParser.RULE_fieldDeclaration) {
                return null
            }
            val n = name ?: return null
            return decl.ownerFqnForMembers()?.let { "$it.$n" }
        }
}

/**
 * A field or local-variable declaration STATEMENT. Owns no name of its own when
 * it declares several (see [CajetaVariableName]); delegates when it declares
 * exactly one, so the single-declarator common case behaves as a plain named
 * element.
 */
class CajetaVariableDeclaration(node: ASTNode) : CajetaNamedElement(node) {

    /** The declared names, in source order — never from initializer bodies. */
    val declarators: List<CajetaVariableName>
        get() = children
            .filter { ruleIndexOf(it) == CajetaParser.RULE_variableDeclarators }
            .flatMap { it.children.toList() }                  // variableDeclarator
            .mapNotNull { d ->
                d.children.firstOrNull { it is CajetaVariableName } as? CajetaVariableName
            }

    override fun getNameIdentifier(): PsiElement? {
        // `var x = ...` locals name a bare identifier, no declarator.
        directChildRule(this, CajetaParser.RULE_identifier)?.let { return it }
        val ds = declarators
        return if (ds.size == 1) ds[0].nameIdentifier else null
    }

    internal fun ownerFqnForMembers(): String? = ownerFqn()
}

/** A formal parameter (incl. the vararg tail). One name, held by its declarator. */
class CajetaParameterDeclaration(node: ASTNode) : CajetaNamedElement(node) {
    override fun getNameIdentifier(): PsiElement? =
        (children.firstOrNull { it is CajetaVariableName } as? CajetaVariableName)
            ?.nameIdentifier
}

/** A type parameter (`<T>`). */
class CajetaTypeParameterDeclaration(node: ASTNode) : CajetaNamedElement(node) {
    override fun getNameIdentifier(): PsiElement? =
        directChildRule(this, CajetaParser.RULE_identifier)
}

/** An enum constant. */
class CajetaEnumConstantDeclaration(node: ASTNode) : CajetaNamedElement(node) {
    override fun getNameIdentifier(): PsiElement? =
        directChildRule(this, CajetaParser.RULE_identifier)

    override val fqn: String?
        get() {
            val n = name ?: return null
            return ownerFqn()?.let { "$it.$n" }
        }
}

/**
 * The factory `CajetaParserDefinition.createElement` delegates to: typed
 * elements for the declaring rules (spec §3.0.2 + the two rules that actually
 * hold interface-method and annotation-element identifiers), [ANTLRPsiNode]
 * for everything else.
 */
object CajetaPsiElementFactory {

    fun createElement(node: ASTNode): PsiElement = when (ruleIndexOf(node)) {
        CajetaParser.RULE_classDeclaration,
        CajetaParser.RULE_interfaceDeclaration,
        CajetaParser.RULE_enumDeclaration,
        CajetaParser.RULE_recordDeclaration,
        CajetaParser.RULE_viewDeclaration,
        CajetaParser.RULE_annotationTypeDeclaration,
        -> CajetaTypeDeclaration(node)

        CajetaParser.RULE_methodDeclaration,
        CajetaParser.RULE_constructorDeclaration,
        CajetaParser.RULE_destructorDeclaration,
        CajetaParser.RULE_interfaceCommonBodyDeclaration,
        CajetaParser.RULE_annotationMethodRest,
        -> CajetaMemberDeclaration(node)

        CajetaParser.RULE_operatorOverloadDeclaration,
        -> CajetaOperatorDeclaration(node)

        CajetaParser.RULE_fieldDeclaration,
        CajetaParser.RULE_localVariableDeclaration,
        -> CajetaVariableDeclaration(node)

        CajetaParser.RULE_variableDeclaratorId,
        -> CajetaVariableName(node)

        CajetaParser.RULE_formalParameter,
        CajetaParser.RULE_lastFormalParameter,
        -> CajetaParameterDeclaration(node)

        CajetaParser.RULE_typeParameter,
        -> CajetaTypeParameterDeclaration(node)

        CajetaParser.RULE_enumConstant,
        -> CajetaEnumConstantDeclaration(node)

        // Unit 7: identifiers carry the xref reference adapter.
        CajetaParser.RULE_identifier,
        -> CajetaIdentifier(node)

        else -> ANTLRPsiNode(node)
    }
}
