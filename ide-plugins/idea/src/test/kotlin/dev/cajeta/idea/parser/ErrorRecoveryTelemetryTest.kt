package dev.cajeta.idea.parser

import dev.cajeta.idea.parser.antlr.CajetaLexer
import dev.cajeta.idea.parser.antlr.CajetaParser
import org.antlr.v4.runtime.CharStreams
import org.antlr.v4.runtime.CommonTokenStream
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * W4b: the error-recovery telemetry aggregator, plus an end-to-end check that
 * CajetaErrorStrategy actually feeds it when the real parser recovers from
 * malformed input. The aggregator itself is plain data, so no platform fixture.
 */
class ErrorRecoveryTelemetryTest {

    @Before
    fun clear() = ErrorRecoveryTelemetry.reset()

    @Test
    fun talliesPerTokenAndAnchorVsNonAnchor() {
        ErrorRecoveryTelemetry.record(CajetaLexer.RBRACE, onAnchor = true)
        ErrorRecoveryTelemetry.record(CajetaLexer.RBRACE, onAnchor = true)
        ErrorRecoveryTelemetry.record(CajetaLexer.SEMI, onAnchor = true)
        ErrorRecoveryTelemetry.record(CajetaLexer.IDENTIFIER, onAnchor = false)

        assertEquals(4L, ErrorRecoveryTelemetry.total())
        assertEquals(3L, ErrorRecoveryTelemetry.anchorLandings())
        assertEquals(1L, ErrorRecoveryTelemetry.nonAnchorLandings())
        assertEquals(2L, ErrorRecoveryTelemetry.countFor(CajetaLexer.RBRACE))

        // snapshot is busiest-first and carries symbolic names when given vocab.
        val snap = ErrorRecoveryTelemetry.snapshot(CajetaLexer.VOCABULARY)
        assertEquals(CajetaLexer.RBRACE, snap.first().tokenType)
        assertEquals(2L, snap.first().count)
        assertEquals("RBRACE", snap.first().tokenName)
    }

    @Test
    fun resetClearsEverything() {
        ErrorRecoveryTelemetry.record(CajetaLexer.SEMI, onAnchor = true)
        ErrorRecoveryTelemetry.reset()
        assertEquals(0L, ErrorRecoveryTelemetry.total())
        assertTrue(ErrorRecoveryTelemetry.snapshot().isEmpty())
    }

    @Test
    fun realParserRecoveryFeedsTelemetry() {
        // Garbage inside a method body forces the parser past single-token
        // repair into recover(), which syncs to an anchor (the closing braces).
        val src = """
            class Foo {
              void bar() { @ @ @ ) ) ) }
            }
        """.trimIndent()

        val lexer = CajetaLexer(CharStreams.fromString(src))
        val parser = CajetaParser(CommonTokenStream(lexer))
        parser.removeErrorListeners()                 // silence console spam
        parser.errorHandler = CajetaErrorStrategy()
        parser.compilationUnit()

        assertTrue(
            "expected at least one recovery to be recorded, got ${ErrorRecoveryTelemetry.total()}",
            ErrorRecoveryTelemetry.total() >= 1,
        )
    }
}
