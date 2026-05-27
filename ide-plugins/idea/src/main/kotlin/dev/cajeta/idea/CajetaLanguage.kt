package dev.cajeta.idea

import com.intellij.lang.Language
import dev.cajeta.idea.parser.antlr.CajetaLexer
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
        PSIElementTypeFactory.defineLanguageIElementTypes(
            this,
            CajetaLexer.tokenNames,
            CajetaLexer.ruleNames,
        )
    }
}
