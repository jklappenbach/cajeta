package dev.cajeta.idea.editor

import com.intellij.lang.BracePair
import com.intellij.lang.PairedBraceMatcher
import com.intellij.psi.PsiFile
import com.intellij.psi.tree.IElementType
import dev.cajeta.idea.CajetaLanguage
import dev.cajeta.idea.parser.antlr.CajetaLexer
import org.antlr.intellij.adaptor.lexer.PSIElementTypeFactory

class CajetaBraceMatcher : PairedBraceMatcher {

    override fun getPairs(): Array<BracePair> = PAIRS

    override fun isPairedBracesAllowedBeforeType(lbraceType: IElementType, contextType: IElementType?): Boolean = true

    override fun getCodeConstructStart(file: PsiFile?, openingBraceOffset: Int): Int = openingBraceOffset

    companion object {
        private fun tokenFor(antlrId: Int): IElementType =
            PSIElementTypeFactory.getTokenIElementTypes(CajetaLanguage)[antlrId]

        private val PAIRS: Array<BracePair> = arrayOf(
            BracePair(tokenFor(CajetaLexer.LBRACE), tokenFor(CajetaLexer.RBRACE), true),
            BracePair(tokenFor(CajetaLexer.LPAREN), tokenFor(CajetaLexer.RPAREN), false),
            BracePair(tokenFor(CajetaLexer.LBRACK), tokenFor(CajetaLexer.RBRACK), false),
        )
    }
}
