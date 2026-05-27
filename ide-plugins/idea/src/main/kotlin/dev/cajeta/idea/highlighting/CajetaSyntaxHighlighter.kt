package dev.cajeta.idea.highlighting

import com.intellij.lexer.Lexer
import com.intellij.openapi.editor.DefaultLanguageHighlighterColors as Colors
import com.intellij.openapi.editor.colors.TextAttributesKey
import com.intellij.openapi.fileTypes.SyntaxHighlighterBase
import com.intellij.psi.tree.IElementType
import dev.cajeta.idea.CajetaLanguage
import dev.cajeta.idea.parser.antlr.CajetaLexer
import org.antlr.intellij.adaptor.lexer.ANTLRLexerAdaptor
import org.antlr.intellij.adaptor.lexer.PSIElementTypeFactory

class CajetaSyntaxHighlighter : SyntaxHighlighterBase() {

    override fun getHighlightingLexer(): Lexer =
        ANTLRLexerAdaptor(CajetaLanguage, CajetaLexer(null))

    override fun getTokenHighlights(tokenType: IElementType): Array<TextAttributesKey> {
        val antlrTokenType = AntlrTokenIds.idFor(tokenType) ?: return emptyArray()
        val attr = when (antlrTokenType) {
            // CajetaKeywords.ALL is regenerated from CajetaLexer.g4 by
            // the generateTokenCategories Gradle task. It captures
            // every token whose literal is an alphabetic word —
            // covers both keywords (class, public, …) and primitive
            // type names (int32, char, …).
            in CajetaKeywords.ALL -> Colors.KEYWORD
            in CONSTANTS   -> Colors.KEYWORD
            in NUMBERS     -> Colors.NUMBER
            in STRINGS     -> Colors.STRING
            in OPERATORS   -> Colors.OPERATION_SIGN
            in PARENS      -> Colors.PARENTHESES
            in BRACES      -> Colors.BRACES
            in BRACKETS    -> Colors.BRACKETS
            in PUNCT       -> Colors.DOT
            CajetaLexer.SEMI       -> Colors.SEMICOLON
            CajetaLexer.COMMA      -> Colors.COMMA
            CajetaLexer.AT         -> Colors.METADATA
            CajetaLexer.COMMENT    -> Colors.BLOCK_COMMENT
            CajetaLexer.LINE_COMMENT -> Colors.LINE_COMMENT
            CajetaLexer.IDENTIFIER -> Colors.IDENTIFIER
            else -> return emptyArray()
        }
        return arrayOf(attr)
    }

    companion object {
        // BOOL_LITERAL is defined as `'true' | 'false'` in the
        // grammar (multi-alternative), so the codegen heuristic
        // (single-literal alphabetic word) doesn't catch it. Listed
        // here explicitly so it still colors as a keyword.
        private val CONSTANTS = setOf(
            CajetaLexer.BOOL_LITERAL,
        )

        private val NUMBERS = setOf(
            CajetaLexer.DECIMAL_LITERAL, CajetaLexer.HEX_LITERAL,
            CajetaLexer.OCT_LITERAL, CajetaLexer.BINARY_LITERAL,
            CajetaLexer.FLOAT_LITERAL, CajetaLexer.HEX_FLOAT_LITERAL,
        )

        private val STRINGS = setOf(
            CajetaLexer.CHAR_LITERAL, CajetaLexer.STRING_LITERAL,
            CajetaLexer.TEXT_BLOCK,
        )

        private val OPERATORS = setOf(
            CajetaLexer.ASSIGN, CajetaLexer.GT, CajetaLexer.LT,
            CajetaLexer.BANG, CajetaLexer.TILDE, CajetaLexer.QUESTION,
            CajetaLexer.COLON, CajetaLexer.EQUAL, CajetaLexer.LE,
            CajetaLexer.GE, CajetaLexer.NOTEQUAL, CajetaLexer.AND,
            CajetaLexer.OR, CajetaLexer.INC, CajetaLexer.DEC,
            CajetaLexer.ADD, CajetaLexer.SUB, CajetaLexer.MUL,
            CajetaLexer.DIV, CajetaLexer.BITAND, CajetaLexer.BITOR,
            CajetaLexer.CARET, CajetaLexer.MOD,
            CajetaLexer.ADD_ASSIGN, CajetaLexer.SUB_ASSIGN,
            CajetaLexer.MUL_ASSIGN, CajetaLexer.DIV_ASSIGN,
            CajetaLexer.AND_ASSIGN, CajetaLexer.OR_ASSIGN,
            CajetaLexer.XOR_ASSIGN, CajetaLexer.MOD_ASSIGN,
            CajetaLexer.LSHIFT_ASSIGN, CajetaLexer.RSHIFT_ASSIGN,
            CajetaLexer.URSHIFT_ASSIGN,
            CajetaLexer.ARROW, CajetaLexer.COLONCOLON, CajetaLexer.REFERENCE,
        )

        private val PARENS = setOf(CajetaLexer.LPAREN, CajetaLexer.RPAREN)
        private val BRACES = setOf(CajetaLexer.LBRACE, CajetaLexer.RBRACE)
        private val BRACKETS = setOf(CajetaLexer.LBRACK, CajetaLexer.RBRACK)
        private val PUNCT = setOf(CajetaLexer.DOT, CajetaLexer.ELLIPSIS)
    }
}

/**
 * Maps from IntelliJ IElementType (as the adaptor delivers them) back
 * to the ANTLR integer token type. The adaptor's TokenIElementType
 * carries this on the instance; we cache the reverse map once.
 */
private object AntlrTokenIds {
    private val map: Map<IElementType, Int> by lazy {
        PSIElementTypeFactory.getTokenIElementTypes(CajetaLanguage)
            .associate { it as IElementType to it.antlrTokenType }
    }
    fun idFor(t: IElementType): Int? = map[t]
}
