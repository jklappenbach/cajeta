package dev.cajeta.idea.lint

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.file.Path

/**
 * lint-own-archive-classpath Unit 5 — the warm daemon's start-time context.
 *
 * Source root and classpath are server STARTUP flags, not per-request fields
 * (`LintServerCore.kt:18`). Unit 3 put the project's own archive on the classpath
 * the client passes, which is enough for a one-shot lint and not enough for a
 * daemon: a server spawned before `cajeta build` keeps answering from the context
 * it started with, so the archive that now exists is invisible to it and the
 * project's own types stay red until the IDE restarts.
 *
 * The binary-identity check (lint-server Unit 5) already had exactly this shape
 * for a rebuilt COMPILER. This adds the same judgement for a rebuilt ARCHIVE and
 * feeds the same teardown-and-respawn path.
 *
 * Everything here is the pure protocol — the transport is faked and the context
 * reading is a scripted function, so no compiler and no IntelliJ platform.
 */
class LintServerContextTest {

    /** As in [LintServerStalenessTest]: ready, then a `done` per request id, so a
     *  test that restarts a server needn't script ids across the restart. */
    private class EchoTransport(private val readyLine: String = READY) : LintServerTransport {
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

    private companion object {
        const val READY =
            """{"kind":"server","proto":{"major":1,"minor":1},"state":"ready"}"""

        /** A ready record that also stamps the binary it is running (§2.8). */
        fun readyWith(binaryId: String): String =
            """{"kind":"server","proto":{"major":1,"minor":1},"state":"ready",""" +
            """"binary":{"path":"/x/cajeta","id":"$binaryId","size":360}}"""

        val REQ = LintServerRequest("staged.cajeta", null, false)
    }

    // ---- 5.1.1 the archive reaches the server's start-time flags ------------

    /** 5.1.1 — the daemon is spawned with the classpath it was handed, so the
     *  project's own archive is part of the context it primes. Without this the
     *  Unit 3 fix is invisible whenever `useLintServer` is on. */
    @Test
    fun theServerArgvCarriesTheOwnArchive() {
        val own = Path.of("/p/build/archive/lib-0.2.0.cja")
        val argv = LintServerContext.argv(
            "/usr/bin/cajeta", "/p/test/src", listOf(Path.of("/p/.cajeta/dep.cja"), own))

        assertEquals(listOf("/usr/bin/cajeta", "--lint-server"), argv.take(2))
        assertTrue("the source root must be a start-time flag; got $argv",
                   argv.windowed(2).contains(listOf("--source-root", "/p/test/src")))
        val cp = argv.single { it.startsWith("--classpath=") }
        assertTrue("the own archive must be on the server's classpath; got $cp",
                   cp.contains(own.toString()))
        assertTrue("...and the dependencies must not be dropped for it; got $cp",
                   cp.contains("/p/.cajeta/dep.cja"))
    }

    /** An empty classpath emits no flag at all rather than `--classpath=`, which
     *  the compiler would read as one empty entry. */
    @Test
    fun anEmptyClasspathEmitsNoFlag() {
        val argv = LintServerContext.argv("/usr/bin/cajeta", "/p/src", emptyList())
        assertTrue("got $argv", argv.none { it.startsWith("--classpath") })
    }

    // ---- the context identity itself ---------------------------------------

    /** The identity moves when the archive's bytes are replaced in place — the
     *  case the daemon cannot otherwise see, because the PATH is unchanged. */
    @Test
    fun theIdentityMovesWhenAnArchiveIsRebuiltInPlace() {
        val p = Path.of("/p/build/archive/lib.cja")
        val before = LintServerContext.identity(listOf(p)) { "1000|400" }
        val after = LintServerContext.identity(listOf(p)) { "2000|410" }
        assertNotEquals("a rebuilt archive must read as a different context", before, after)
    }

    /** ...and is stable when nothing moved. A context that flaps would restart
     *  the daemon on every keystroke, which is worse than the bug. */
    @Test
    fun theIdentityIsStableWhenNothingMoved() {
        val cp = listOf(Path.of("/p/a.cja"), Path.of("/p/b.cja"))
        assertEquals(
            LintServerContext.identity(cp) { "7|7" },
            LintServerContext.identity(cp) { "7|7" },
        )
    }

    /** An absent archive reads as a real value, not as "cannot tell": the
     *  archive appearing and the archive vanishing are both context changes and
     *  both must be judged, so the reading may never be null for a missing file. */
    @Test
    fun anAbsentArchiveHasItsOwnStableIdentity() {
        val p = Path.of("/nowhere/lib.cja")
        val absent = LintServerContext.identity(listOf(p))
        assertEquals("a missing file must read the same way twice",
                     absent, LintServerContext.identity(listOf(p)))
        assertNotEquals("...and differently from a present one",
                        absent, LintServerContext.identity(listOf(p)) { "1|1" })
    }

    /** The identity distinguishes classpaths that differ only in ORDER, because
     *  the compiler resolves in classpath order — two orders are two contexts. */
    @Test
    fun orderIsPartOfTheContext() {
        val a = Path.of("/p/a.cja")
        val b = Path.of("/p/b.cja")
        assertNotEquals(
            LintServerContext.identity(listOf(a, b)) { "1|1" },
            LintServerContext.identity(listOf(b, a)) { "1|1" },
        )
    }

    // ---- 5.1.3 / 5.1.4 / 5.1.5 the restart -------------------------------

    /** 5.1.3 — `cajeta build` lands under a live daemon. The archive is replaced
     *  at the same path, so nothing in the daemon's argv changed and only its
     *  CONTENT did; the next lint must not come from the old server. */
    @Test
    fun anArchiveRebuiltUnderALiveServerRestartsIt() {
        val spawned = mutableListOf<EchoTransport>()
        var stamp = "1000|400"
        val core = LintServerCore(
            spawn = { EchoTransport().also { spawned.add(it) } },
            contextIdentityOf = { stamp },
        )

        assertTrue(core.lint(REQ) is LintServerCore.Result.Payload)
        assertEquals(1, spawned.size)

        stamp = "2000|412"                             // the rebuild lands

        assertTrue(core.lint(REQ) is LintServerCore.Result.Payload)
        assertEquals("a server primed on the old archive must be replaced",
                     2, spawned.size)
        assertTrue("the stale server must be torn down", spawned[0].closed)
        assertEquals("the fresh server answered the request", 1, spawned[1].written.size)
    }

    /** 5.1.4 — the first build of a project that had no archive at all. The
     *  classpath itself grows an entry here, so this is the same judgement on a
     *  different input, and it must reach the same restart. */
    @Test
    fun anArchiveThatAppearsRestartsTheServer() {
        val spawned = mutableListOf<EchoTransport>()
        var classpath = listOf<Path>()
        val core = LintServerCore(
            spawn = { EchoTransport().also { spawned.add(it) } },
            contextIdentityOf = { LintServerContext.identity(classpath) { "1|1" } },
        )

        core.lint(REQ)
        assertEquals(1, spawned.size)

        classpath = listOf(Path.of("/p/build/archive/lib.cja"))   // first build

        core.lint(REQ)
        assertEquals("an archive appearing must restart the daemon", 2, spawned.size)
    }

    /** 5.1.5 — the control that keeps 5.1.3 from passing for the wrong reason.
     *  Ten edits against an unchanged archive, one server. */
    @Test
    fun anUnchangedContextKeepsTheSameServer() {
        val spawned = mutableListOf<EchoTransport>()
        val core = LintServerCore(
            spawn = { EchoTransport().also { spawned.add(it) } },
            contextIdentityOf = { "1000|400" },
        )

        repeat(10) { assertTrue(core.lint(REQ) is LintServerCore.Result.Payload) }
        assertEquals("an unchanged context must not restart the server", 1, spawned.size)
    }

    /** 5.3.2 — one rebuild costs one restart, not one per edit afterwards. */
    @Test
    fun oneRebuildCostsOneRestart() {
        val spawned = mutableListOf<EchoTransport>()
        var stamp = "1000|400"
        val core = LintServerCore(
            spawn = { EchoTransport().also { spawned.add(it) } },
            contextIdentityOf = { stamp },
        )

        repeat(5) { core.lint(REQ) }
        stamp = "2000|412"
        repeat(5) { core.lint(REQ) }

        assertEquals("ten edits across one rebuild must spawn exactly twice",
                     2, spawned.size)
    }

    /** The change is invisible otherwise, and its symptom ("the build didn't
     *  take") blames the wrong component — the same reasoning that put a log
     *  line on the binary-identity restart. Reported once per change, not per
     *  edit. */
    @Test
    fun theRestartNamesTheOldAndTheNewContext() {
        val seen = mutableListOf<Pair<String, String?>>()
        var stamp = "1000|400"
        val core = LintServerCore(
            spawn = { EchoTransport() },
            contextIdentityOf = { stamp },
            onContextChanged = { was, now -> seen.add(was to now) },
        )

        core.lint(REQ)
        stamp = "2000|412"
        core.lint(REQ)
        core.lint(REQ)

        assertEquals("one change, one report", 1, seen.size)
        assertEquals("1000|400" to "2000|412", seen[0])
    }

    // ---- the instrument, and the ways it can be broken ---------------------

    /** Compatibility, and the shape [LintServerCore] already keeps for the
     *  binary check: a core configured without a context reading behaves exactly
     *  as it did before this unit. */
    @Test
    fun aCoreWithNoContextFunctionNeverRestarts() {
        val spawned = mutableListOf<EchoTransport>()
        val core = LintServerCore(spawn = { EchoTransport().also { spawned.add(it) } })

        repeat(3) { core.lint(REQ) }
        assertEquals(1, spawned.size)
    }

    /** A reading that throws is a broken instrument, not a verdict. It must not
     *  escape to the annotator and must not restart a healthy daemon. */
    @Test
    fun aContextReadingThatThrowsDoesNotEscapeOrRestart() {
        val spawned = mutableListOf<EchoTransport>()
        val core = LintServerCore(
            spawn = { EchoTransport().also { spawned.add(it) } },
            contextIdentityOf = { throw RuntimeException("stat blew up") },
        )

        repeat(4) { assertTrue(core.lint(REQ) is LintServerCore.Result.Payload) }
        assertEquals("a broken reading must settle, not churn", 1, spawned.size)
    }

    /** ...and the same for a reading that declines to answer. Unlike the binary
     *  check this needs no latch: the value is compared against OUR OWN earlier
     *  reading, so a fresh server always agrees with itself and the permanent
     *  disagreement that check has to defend against cannot arise here. */
    @Test
    fun aContextReadingThatCannotBeTakenNeverRestarts() {
        val spawned = mutableListOf<EchoTransport>()
        val core = LintServerCore(
            spawn = { EchoTransport().also { spawned.add(it) } },
            contextIdentityOf = { null },
        )

        repeat(4) { core.lint(REQ) }
        assertEquals(1, spawned.size)
    }

    /** 5.2.3 — a context that changes on every edit against a compiler that will
     *  not start must not launch a server per keystroke. The existing failure
     *  backoff covers it, and this pins that the context path did not route
     *  around it. */
    @Test
    fun aRebuildLoopAgainstAFailingSpawnIsBackedOff() {
        var spawns = 0
        var clock = 0L
        var stamp = 0
        val core = LintServerCore(
            spawn = { spawns++; throw java.io.IOException("mid-relink") },
            backoffNanos = 2_000_000_000L,
            now = { clock },
            contextIdentityOf = { "stamp-${stamp++}" },   // changes every reading
        )

        repeat(20) { core.lint(REQ) }
        assertEquals("a failing spawn must be backed off, not retried per edit",
                     1, spawns)

        clock = 3_000_000_000L                            // past the backoff
        core.lint(REQ)
        assertEquals("...and retried once the backoff elapses", 2, spawns)
    }

    /** A rebuilt compiler and a rebuilt archive at the same moment is one
     *  restart, not two — the checks feed one teardown. */
    @Test
    fun aSimultaneousBinaryAndContextChangeRestartsOnce() {
        val spawned = mutableListOf<EchoTransport>()
        var binary = "sha256:old"
        var stamp = "1000|400"
        val core = LintServerCore(
            spawn = { EchoTransport(readyWith(binary)).also { spawned.add(it) } },
            identityOf = { binary },
            contextIdentityOf = { stamp },
        )

        core.lint(REQ)
        binary = "sha256:new"
        stamp = "2000|412"
        core.lint(REQ)

        assertEquals(2, spawned.size)
    }
}
