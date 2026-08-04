package dev.cajeta.idea.lint

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * lint-server-spec §2 (plan 4.1.1–4.1.3): the pure protocol — ready/done
 * framing, id correlation, unknown-major refusal, and server-death fallback
 * with backoff. The transport is faked (scripted stdout lines, recorded stdin)
 * so nothing but the protocol is under test.
 */
class LintServerCoreTest {

    private class FakeTransport(
        val lines: ArrayDeque<String> = ArrayDeque(),
        var alive: Boolean = true,
    ) : LintServerTransport {
        val written = mutableListOf<String>()
        override fun writeLine(line: String) {
            if (!alive) throw IllegalStateException("dead")
            written.add(line)
        }
        override fun readLine(): String? {
            if (lines.isEmpty()) { alive = false; return null }  // EOF
            return lines.removeFirst()
        }
        override fun isAlive(): Boolean = alive
        override fun close() { alive = false }
    }

    private val READY = """{"kind":"server","proto":{"major":1,"minor":0},"state":"ready"}"""
    private val DIAG = """{"kind":"diagnostic","severity":"error","message":"boom","line":1,"column":1}"""
    private val XREF = """{"kind":"xref","rel":"defines","record":{"symbol":"demo.A"}}"""

    // 4.1.1 — ready/done framing parsed; the payload is the lines between; ids
    // correlate across successive requests.
    @Test
    fun parsesFramingAndCorrelatesIds() {
        val t = FakeTransport(ArrayDeque(listOf(
            READY,
            DIAG, XREF, """{"kind":"done","id":1}""",
            DIAG, """{"kind":"done","id":2}""",
        )))
        val core = LintServerCore(spawn = { t })

        val r1 = core.lint(LintServerRequest("staged.cajeta", "Orig.cajeta", true))
        assertTrue(r1 is LintServerCore.Result.Payload)
        val payload = (r1 as LintServerCore.Result.Payload).text
        assertTrue(payload.contains("\"kind\":\"diagnostic\""))
        assertTrue(payload.contains("\"kind\":\"xref\""))
        // the done marker is NOT part of the payload
        assertFalse(payload.contains("\"kind\":\"done\""))
        // request carried id 1 and the staged file + shadow + emitXref
        assertTrue(t.written[0].contains("\"id\":1"))
        assertTrue(t.written[0].contains("\"file\":\"staged.cajeta\""))
        assertTrue(t.written[0].contains("\"shadow\":\"Orig.cajeta\""))
        assertTrue(t.written[0].contains("\"emitXref\":true"))

        val r2 = core.lint(LintServerRequest("staged.cajeta", null, false))
        assertTrue(r2 is LintServerCore.Result.Payload)
        // the warm server was reused (no second ready read), id incremented
        assertTrue(t.written[1].contains("\"id\":2"))
        assertFalse(t.written[1].contains("\"shadow\""))
        assertFalse(t.written[1].contains("\"emitXref\":true"))
        assertEquals(2, t.written.size)
    }

    // 4.1.2 — an unknown protocol major: never send a request, latch Unsupported,
    // and don't respawn on the next call (one notification's worth of noise).
    @Test
    fun refusesUnknownProtocolMajor() {
        var spawns = 0
        val t = FakeTransport(ArrayDeque(listOf(
            """{"kind":"server","proto":{"major":99,"minor":0},"state":"ready"}""",
        )))
        val core = LintServerCore(spawn = { spawns++; t })

        val r1 = core.lint(LintServerRequest("f.cajeta", null, false))
        assertEquals(LintServerCore.Result.Unsupported, r1)
        assertTrue("must not send a lint request to an unreadable server",
                   t.written.isEmpty())

        val r2 = core.lint(LintServerRequest("f.cajeta", null, false))
        assertEquals(LintServerCore.Result.Unsupported, r2)
        assertEquals("latched: no respawn after an unknown major", 1, spawns)
    }

    // 4.1.3 — server death mid-request falls back one-shot; the next call is
    // held off by the backoff; after it elapses the server respawns and serves.
    @Test
    fun serverDeathFallsBackThenRespawnsAfterBackoff() {
        var spawns = 0
        // first transport: hands out ready, then EOF (dies mid-request).
        val dead = FakeTransport(ArrayDeque(listOf(READY)))
        // second transport: a healthy server. The client's id counter is
        // monotonic across the respawn, so by the third request it sends id 2 —
        // a real server echoes whatever id it received.
        val healthy = FakeTransport(ArrayDeque(listOf(
            READY, DIAG, """{"kind":"done","id":2}""",
        )))
        val queue = ArrayDeque(listOf<LintServerTransport>(dead, healthy))
        var clock = 0L
        val core = LintServerCore(
            spawn = { spawns++; queue.removeFirst() },
            backoffNanos = 1_000L,
            now = { clock },
        )

        // request against the dead server: ready read, request written, then EOF.
        val r1 = core.lint(LintServerRequest("f.cajeta", null, false))
        assertEquals(LintServerCore.Result.Failed, r1)
        assertEquals(1, spawns)

        // immediate retry: still within backoff → Failed, NO respawn.
        clock = 500L
        val r2 = core.lint(LintServerRequest("f.cajeta", null, false))
        assertEquals(LintServerCore.Result.Failed, r2)
        assertEquals("backoff suppresses the respawn", 1, spawns)

        // after the backoff elapses: respawn onto the healthy server, serve.
        clock = 2_000L
        val r3 = core.lint(LintServerRequest("f.cajeta", null, false))
        assertTrue(r3 is LintServerCore.Result.Payload)
        assertTrue((r3 as LintServerCore.Result.Payload).text.contains("diagnostic"))
        assertEquals(2, spawns)
    }

    // A malformed-request error record from the server is a fallback, not a hang.
    @Test
    fun serverErrorRecordFallsBack() {
        val t = FakeTransport(ArrayDeque(listOf(
            READY,
            """{"kind":"error","id":1,"code":"boom","message":"nope"}""",
        )))
        val core = LintServerCore(spawn = { t }, backoffNanos = 0L)
        val r = core.lint(LintServerRequest("f.cajeta", null, false))
        assertEquals(LintServerCore.Result.Failed, r)
    }
}
