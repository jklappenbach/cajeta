package dev.cajeta.idea.parser

import dev.cajeta.idea.parser.antlr.CajetaLexer
import dev.cajeta.idea.parser.antlr.CajetaParser
import org.antlr.v4.runtime.BaseErrorListener
import org.antlr.v4.runtime.CharStreams
import org.antlr.v4.runtime.CommonTokenStream
import org.antlr.v4.runtime.RecognitionException
import org.antlr.v4.runtime.Recognizer

/**
 * Replays a source string the way a user types it — one character at a time —
 * parsing every prefix through the real lexer/parser + [CajetaErrorStrategy]
 * (W4c). The invariant it guards: incremental parsing must NEVER throw on any
 * partial input (it may only report errors and recover gracefully), and a
 * complete valid file must parse error-free. This catches ANTLR error-recovery
 * regressions and exceptions-during-incremental-parse that the per-file
 * open-and-render tests don't surface — intermediate prefixes of valid code
 * naturally carry errors (the input is incomplete), so the assertion for valid
 * fixtures is "never crashes" + "clean when complete", not "no error ever".
 */
object TypingSimulator {

    data class Step(val prefixLen: Int, val syntaxErrors: Int, val threw: Throwable?) {
        val crashed: Boolean get() = threw != null
    }

    /** Parse every prefix `s[0,1)`, `s[0,2)`, … `s` (one keystroke each). */
    fun simulate(source: String): List<Step> =
        (1..source.length).map { parsePrefix(source.substring(0, it)) }

    /** Parse a single prefix. Never throws — any exception is captured in [Step]. */
    fun parsePrefix(text: String): Step = try {
        val counter = ErrorCounter()
        val lexer = CajetaLexer(CharStreams.fromString(text)).apply {
            removeErrorListeners(); addErrorListener(counter)
        }
        val parser = CajetaParser(CommonTokenStream(lexer)).apply {
            removeErrorListeners(); addErrorListener(counter)
            errorHandler = CajetaErrorStrategy()
        }
        parser.compilationUnit()
        Step(text.length, counter.count, null)
    } catch (t: Throwable) {
        Step(text.length, -1, t)
    }

    private class ErrorCounter : BaseErrorListener() {
        var count = 0
        override fun syntaxError(
            recognizer: Recognizer<*, *>?, offendingSymbol: Any?, line: Int,
            charPositionInLine: Int, msg: String?, e: RecognitionException?,
        ) {
            count++
        }
    }
}
