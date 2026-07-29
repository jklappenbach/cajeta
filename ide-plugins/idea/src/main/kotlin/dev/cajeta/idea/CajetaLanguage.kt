package dev.cajeta.idea

import com.intellij.lang.Language
import dev.cajeta.idea.parser.antlr.CajetaLexer
import dev.cajeta.idea.parser.antlr.CajetaParser
import org.antlr.intellij.adaptor.lexer.PSIElementTypeFactory

object CajetaLanguage : Language("Cajeta") {
    init {
        // Register token + rule IElementTypes for this language BEFORE
        // anyone creates an ANTLRLexerAdaptor against it — the adaptor
        // looks up the per-language token-element-type list during
        // every Token→IElementType conversion and NPEs if it's missing.
        // Putting this in the Language's init block guarantees it runs
        // first, since every extension entry point references the
        // language.
        //
        // Rule element types are indexed by PARSER rule index (the adaptor's
        // tree converter does ruleElementTypes.get(ctx.ruleIndex)), so the
        // names registered here must be the PARSER's. This used to pass
        // CajetaLexer.ruleNames — the indices still lined up, but every rule
        // node carried some lexer rule's NAME ("EXPORTS" for `identifier"),
        // which is why the pre-Unit-5 structure view's rule-name string
        // matching never matched anything (ide-symbol-index plan 5.4).
        PSIElementTypeFactory.defineLanguageIElementTypes(
            this,
            CajetaLexer.tokenNames,
            CajetaParser.ruleNames,
        )
    }
}
