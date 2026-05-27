package dev.cajeta.idea.parser

import com.intellij.psi.tree.IElementType
import dev.cajeta.idea.CajetaLanguage
import org.antlr.intellij.adaptor.lexer.PSIElementTypeFactory
import org.antlr.intellij.adaptor.lexer.RuleIElementType

object CajetaElementTypes {
    init {
        // Force CajetaTokenTypes to initialize first so token + rule types
        // are registered against the language.
        CajetaTokenTypes.TOKENS
    }

    val RULES: List<RuleIElementType> =
        PSIElementTypeFactory.getRuleIElementTypes(CajetaLanguage)

    val BY_NAME: Map<String, RuleIElementType> =
        RULES.associateBy { it.toString() }

    fun ruleByName(name: String): IElementType? = BY_NAME[name]
}
