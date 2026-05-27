package dev.cajeta.idea.parser

import dev.cajeta.idea.CajetaLanguage
import org.antlr.intellij.adaptor.lexer.PSIElementTypeFactory
import org.antlr.intellij.adaptor.lexer.TokenIElementType

object CajetaTokenTypes {
    // Registration of element types lives in CajetaLanguage's init
    // block so it runs before any lexer adapter is constructed.
    // Touching CajetaLanguage here re-triggers it if needed.
    private val language = CajetaLanguage

    val TOKENS: List<TokenIElementType> =
        PSIElementTypeFactory.getTokenIElementTypes(language)

    val BY_NAME: Map<String, TokenIElementType> =
        TOKENS.associateBy { it.toString() }
}
