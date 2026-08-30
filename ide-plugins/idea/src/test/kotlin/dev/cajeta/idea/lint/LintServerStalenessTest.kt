package dev.cajeta.idea.lint

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * lint-server plan Unit 5 (spec §2.8): a resident server keeps serving the code
 * it started with. Measured 2026-08-29 on `samples/tour` — the daemon started
 * 15:35:38, the binary carrying the xref column fix was built 15:46:52, and
 * every per-edit lint after that answered from the old image, overwrote the
 * good shard, and broke Ctrl-click again. It read as "worked once, then
 * reverted", which points at the wrong component entirely.
 *
 * The server now stamps its own binary's content identity on the ready record;
 * the client compares it against the binary it is configured to call and
 * restarts rather than serving from a stale one. Everything here is the pure
 * protocol: the transport is faked and the identity is a scripted function.
 */
class LintServerStalenessTest {

    /**
     * A server that hands out its ready record, then answers every request with
     * a `done` echoing that request's id. Ids correlate by construction, so a
     * test that restarts a server does not also have to script the id sequence
     * across the restart.
     */
    private class EchoTransport(private val readyLine: String) : LintServerTransport {
        val written = mutableListOf<String>()
        private val pending = ArrayDeque<String>()
        private var sentReady = false
        var alive = true
        var closed = false

        override fun writeLine(line: String) {
            if (!alive) throw IllegalStateException("dead")
            written.add(line)
            val id = Regex("\"id\":(\\d+)").find(line)?.groupValues?.get(1)
            if (id != null) pending.add("""{"kind":"done","id":$id}""")
        }
        override fun readLine(): String? {
            if (!sentReady) { sentReady = true; return readyLine }
            if (pending.isEmpty()) { alive = false; return null }
            return pending.removeFirst()
        }
        override fun isAlive(): Boolean = alive
        override fun close() { alive = false; closed = true }
    }

    private fun ready(id: String?): String =
        if (id == null)
            """{"kind":"server","proto":{"major":1,"minor":0},"state":"ready"}"""
        else
            """{"kind":"server","proto":{"major":1,"minor":1},"state":"ready",""" +
            """"binary":{"path":"/x/cajeta","id":"$id","size":360}}"""

    private val REQ = LintServerRequest("staged.cajeta", null, false)

    // 5.1.a — the compiler is rebuilt under a live daemon. The next lint must
    // NOT come from it: the core tears it down and spawns a fresh one.
    @Test
    fun aServerRunningAReplacedBinaryIsRestartedNotReused() {
        val spawned = mutableListOf<EchoTransport>()
        var current = "sha256:old"
        val core = LintServerCore(
            spawn = { EchoTransport(ready(current)).also { spawned.add(it) } },
            identityOf = { current },
        )

        assertTrue(core.lint(REQ) is LintServerCore.Result.Payload)
        assertEquals(1, spawned.size)

        current = "sha256:new"                       // the rebuild lands

        assertTrue(core.lint(REQ) is LintServerCore.Result.Payload)
        assertEquals("a stale server must be replaced, not reused", 2, spawned.size)
        assertTrue("the stale server must be torn down", spawned[0].closed)
        assertEquals("the fresh server answered the request", 1, spawned[1].written.size)
    }

    // 5.1.b — an unchanged binary must not churn. Ten lints, one server.
    @Test
    fun anUnchangedBinaryKeepsTheSameServer() {
        val spawned = mutableListOf<EchoTransport>()
        val core = LintServerCore(
            spawn = { EchoTransport(ready("sha256:same")).also { spawned.add(it) } },
            identityOf = { "sha256:same" },
        )

        repeat(10) { assertTrue(core.lint(REQ) is LintServerCore.Result.Payload) }
        assertEquals("an unchanged binary must not restart the server", 1, spawned.size)
    }

    // 5.2.c — the failure is invisible otherwise, and its symptom blames the
    // wrong component. One log line, naming both identities.
    @Test
    fun theRestartNamesTheOldAndTheNewIdentity() {
        val seen = mutableListOf<Pair<String, String?>>()
        var current = "sha256:old"
        val core = LintServerCore(
            spawn = { EchoTransport(ready(current)) },
            identityOf = { current },
            onStale = { was, now -> seen.add(was to now) },
        )

        core.lint(REQ)
        current = "sha256:new"
        core.lint(REQ)

        assertEquals(1, seen.size)
        assertEquals("sha256:old" to "sha256:new", seen[0])
    }

    // Compatibility: a server built before this check reports no identity. It
    // cannot be judged, so it must never be judged stale — otherwise every lint
    // against an older compiler restarts a healthy daemon.
    @Test
    fun aServerThatReportsNoIdentityIsNeverJudgedStale() {
        val spawned = mutableListOf<EchoTransport>()
        val core = LintServerCore(
            spawn = { EchoTransport(ready(null)).also { spawned.add(it) } },
            identityOf = { "sha256:whatever" },
        )

        core.lint(REQ)
        core.lint(REQ)
        assertEquals(1, spawned.size)
    }

    // 5.1.c — the binary is gone (mid-relink, or a moved install). The check
    // must not throw and must not serve on as if nothing happened; the fresh
    // spawn fails, which folds to the one-shot fallback the caller already has.
    @Test
    fun aSinceDeletedBinaryIsStaleNotACrash() {
        var deleted = false
        var spawns = 0
        val core = LintServerCore(
            spawn = {
                spawns++
                if (deleted) throw java.io.IOException("no such file")
                EchoTransport(ready("sha256:old"))
            },
            identityOf = { if (deleted) null else "sha256:old" },
        )

        assertTrue(core.lint(REQ) is LintServerCore.Result.Payload)
        deleted = true

        assertEquals(LintServerCore.Result.Failed, core.lint(REQ))
        assertEquals("the stale server must not be served from", 2, spawns)
    }

    // A reading that cannot decide must not thrash. If a FRESH server disagrees
    // with the file we believe we are calling, the two are not looking at the
    // same bytes (a wrapper script, a second install) — checking again would
    // restart on every keystroke, so the check latches off and says so once.
    @Test
    fun aCheckThatCannotAgreeWithAFreshServerStopsChecking() {
        var spawns = 0
        val disabled = mutableListOf<Pair<String, String?>>()
        val core = LintServerCore(
            spawn = { spawns++; EchoTransport(ready("sha256:server-side")) },
            identityOf = { "sha256:client-side" },     // permanently disagrees
            onCheckDisabled = { was, now -> disabled.add(was to now) },
        )

        repeat(3) { core.lint(REQ) }

        assertEquals("a disagreement must not restart on every edit", 1, spawns)
        assertEquals(1, disabled.size)
        assertEquals("sha256:server-side" to "sha256:client-side", disabled[0])
    }

    // The identity is read from the wire, not assumed: a core with no identity
    // function configured behaves exactly as it did before Unit 5.
    @Test
    fun aCoreWithNoIdentityFunctionNeverRestarts() {
        val spawned = mutableListOf<EchoTransport>()
        val core = LintServerCore(
            spawn = { EchoTransport(ready("sha256:whatever")).also { spawned.add(it) } })

        core.lint(REQ)
        core.lint(REQ)
        assertEquals(1, spawned.size)
    }

    // A broken instrument is not a verdict: a throwing identity function must
    // not escape to the annotator, and must not restart on every edit either.
    @Test
    fun anIdentityFunctionThatThrowsDoesNotEscapeAndSettles() {
        var spawns = 0
        val core = LintServerCore(
            spawn = { spawns++; EchoTransport(ready("sha256:old")) },
            identityOf = { throw RuntimeException("stat blew up") },
        )

        repeat(4) { core.lint(REQ) }
        assertTrue("a broken check must settle, not restart per edit (spawns=$spawns)",
                   spawns <= 2)
    }
}
