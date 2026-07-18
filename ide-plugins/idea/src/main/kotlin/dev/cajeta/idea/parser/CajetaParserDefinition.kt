package dev.cajeta.idea.parser

import com.intellij.lang.ASTNode
import com.intellij.lang.ParserDefinition
import com.intellij.lang.PsiParser
import com.intellij.lexer.Lexer
import com.intellij.openapi.diagnostic.Logger
import com.intellij.openapi.project.Project
import com.intellij.psi.FileViewProvider
import com.intellij.psi.PsiElement
import com.intellij.psi.PsiFile
import com.intellij.psi.tree.IElementType
import com.intellij.psi.tree.IFileElementType
import com.intellij.psi.tree.TokenSet
import dev.cajeta.idea.CajetaLanguage
import dev.cajeta.idea.parser.antlr.CajetaLexer
import dev.cajeta.idea.parser.antlr.CajetaParser
import dev.cajeta.idea.psi.CajetaPsiElementFactory
import org.antlr.intellij.adaptor.lexer.ANTLRLexerAdaptor
import org.antlr.intellij.adaptor.lexer.PSIElementTypeFactory
import org.antlr.intellij.adaptor.parser.ANTLRParserAdaptor
import org.antlr.v4.runtime.BaseErrorListener
import org.antlr.v4.runtime.Parser as Antlr4Parser
import org.antlr.v4.runtime.RecognitionException
import org.antlr.v4.runtime.Recognizer
import org.antlr.v4.runtime.tree.ParseTree

class CajetaParserDefinition : ParserDefinition {

    override fun createLexer(project: Project?): Lexer =
        ANTLRLexerAdaptor(CajetaLanguage, CajetaLexer(null))

    override fun createParser(project: Project?): PsiParser {
        val parser = CajetaParser(null)
        return object : ANTLRParserAdaptor(CajetaLanguage, parser) {
            override fun parse(p: Antlr4Parser, root: IElementType): ParseTree {
                p.errorHandler = CajetaErrorStrategy()
                // Replace the adaptor's default error listener with one
                // that logs the full error context (rule stack + token
                // around the failure offset). The default listener
                // just emits the bare ANTLR message which is hard to
                // diagnose when the visual span is misleading.
                p.removeErrorListeners()
                p.addErrorListener(LoggingErrorListener)
                return (p as CajetaParser).compilationUnit()
            }
        }
    }

    private object LoggingErrorListener : BaseErrorListener() {
        private val log = Logger.getInstance(LoggingErrorListener::class.java)
        override fun syntaxError(
            recognizer: Recognizer<*, *>?,
            offendingSymbol: Any?,
            line: Int,
            charPositionInLine: Int,
            msg: String?,
            e: RecognitionException?,
        ) {
            val parser = recognizer as? Antlr4Parser
            val rule = parser?.ruleInvocationStack?.joinToString(" > ")?.take(120)
            log.debug("parse error at $line:$charPositionInLine offending='$offendingSymbol' msg='$msg' rule=$rule")
        }
    }

    override fun getFileNodeType(): IFileElementType = FILE

    // ide-symbol-index Unit 5 (plan 5.4): the adaptor's parse-tree→PSI
    // converter advances the PsiBuilder exactly once per parse-tree TERMINAL
    // and trusts the builder to auto-skip whitespace. The builder only skips
    // token types declared here — the default is TokenType.WHITE_SPACE, which
    // our lexer's WS is not. Without this override every terminal advance
    // consumed a WS instead, the rule markers drifted and closed early, and
    // everything after the first composite dangled FLAT under the file node
    // (whitespace even landed inside `identifier` nodes). Nothing consumed
    // rule structure before the named-element layer, so it went unnoticed.
    override fun getWhitespaceTokens(): TokenSet =
        PSIElementTypeFactory.createTokenSet(
            CajetaLanguage,
            CajetaLexer.WS,
        )

    override fun getCommentTokens(): TokenSet {
        // CajetaLexer.g4:232-233 routes COMMENT and LINE_COMMENT to
        // channel(HIDDEN). The adaptor exposes IElementTypes for them
        // regardless of channel.
        return PSIElementTypeFactory.createTokenSet(
            CajetaLanguage,
            CajetaLexer.COMMENT,
            CajetaLexer.LINE_COMMENT,
        )
    }

    override fun getStringLiteralElements(): TokenSet =
        PSIElementTypeFactory.createTokenSet(
            CajetaLanguage,
            CajetaLexer.STRING_LITERAL,
            CajetaLexer.TEXT_BLOCK,
        )

    // ide-symbol-index Unit 5 (spec §3.0.1): typed named elements for the
    // declaring rules, ANTLRPsiNode for everything else.
    override fun createElement(node: ASTNode): PsiElement =
        CajetaPsiElementFactory.createElement(node)

    override fun createFile(viewProvider: FileViewProvider): PsiFile =
        CajetaPsiFile(viewProvider)

    companion object {
        val FILE: IFileElementType = IFileElementType(CajetaLanguage)
    }
}
