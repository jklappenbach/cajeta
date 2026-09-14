package dev.cajeta.idea.lint

import org.junit.After
import org.junit.AfterClass
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.BeforeClass
import org.junit.Test
import java.io.BufferedReader
import java.io.BufferedWriter
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.nio.charset.StandardCharsets
import java.nio.file.Path

/**
 * lint-own-archive-classpath Unit 5 — the fix against a REAL warm daemon.
 *
 * [LintServerContextTest] pins the protocol with a faked transport; this pins
 * that the argv that protocol is spawned with actually resolves the project's own
 * types, because a classpath flag the compiler ignores would pass every pure test
 * and still leave the editor red. The daemon is spawned from
 * [LintServerContext.argv] — the same builder production uses — so the thing
 * under test is the real command line.
 *
 * Skipped wholesale when no compiler is configured (`CAJETA_TEST_COMPILER`).
 */
class WarmLintServerTest {

    companion object {
        private var twoRoot: LintFixture? = null

        @BeforeClass
        @JvmStatic
        fun build() {
            if (LintFixture.compilerAvailable()) twoRoot = LintFixture.twoRoot()
        }

        @AfterClass
        @JvmStatic
        fun cleanUp() {
            twoRoot?.delete()
        }
    }

    private val processes = mutableListOf<Process>()

    @After
    fun killDaemons() {
        processes.forEach { runCatching { it.destroyForcibly() } }
        processes.clear()
    }

    private fun fixture(): LintFixture {
        assumeTrue("no compiler configured (CAJETA_TEST_COMPILER)", LintFixture.compilerAvailable())
        return twoRoot!!
    }

    /** A core bound to a real `cajeta --lint-server` over the given classpath. */
    private fun coreFor(fx: LintFixture, classpath: List<Path>): LintServerCore =
        LintServerCore(spawn = {
            val argv = LintServerContext.argv(
                fx.compilerPath(), fx.abs(fx.testRoot), classpath)
            val p = ProcessBuilder(argv)
                .directory(fx.root.toFile())
                .redirectErrorStream(false)
                .start()
            processes.add(p)
            val stdin = BufferedWriter(OutputStreamWriter(p.outputStream, StandardCharsets.UTF_8))
            val stdout = BufferedReader(InputStreamReader(p.inputStream, StandardCharsets.UTF_8))
            object : LintServerTransport {
                override fun writeLine(line: String) { stdin.write(line); stdin.write("\n"); stdin.flush() }
                override fun readLine(): String? = stdout.readLine()
                override fun isAlive(): Boolean = p.isAlive
                override fun close() {
                    runCatching { stdin.close() }
                    runCatching { stdout.close() }
                    runCatching { p.destroyForcibly() }
                }
            }
        })

    private fun warmUnresolved(core: LintServerCore, file: String): List<String> {
        val r = core.lint(LintServerRequest(file, null, false))
        assertTrue("the warm server must answer; got $r", r is LintServerCore.Result.Payload)
        return LintFixture.unresolvedIn((r as LintServerCore.Result.Payload).text)
    }

    /** 5.1.2 — the whole point of the unit. A test-root file linted through the
     *  warm daemon resolves its own project's types, because the archive was in
     *  the context the daemon primed itself with. */
    @Test(timeout = 600_000)
    fun aWarmServerResolvesTheProjectsOwnTypes() {
        val fx = fixture()
        val core = coreFor(fx, listOf(fx.artifact()))
        assertEquals(
            "the warm server must resolve the project's own types",
            emptyList<String>(), warmUnresolved(core, fx.abs(fx.testFile)),
        )
        core.shutdown()
    }

    /** The does-not-fire control. Spawned WITHOUT the archive the same daemon
     *  reports the project's own type unresolved — so the test above is not
     *  passing because the compiler stopped reporting, or because the payload
     *  was empty. */
    @Test(timeout = 600_000)
    fun aWarmServerWithoutTheArchiveCannotResolveThem() {
        val fx = fixture()
        val core = coreFor(fx, emptyList())
        assertTrue(
            "without the archive the warm server must still report the defect",
            warmUnresolved(core, fx.abs(fx.testFile)).contains("Greeter"),
        )
        core.shutdown()
    }

    /** 5.3.1 — behaviour is identical with the server on and off. Same buffer,
     *  same classpath, same verdict, read by the same parser: a daemon that
     *  quietly resolved MORE than a one-shot would be just as wrong as one that
     *  resolved less. Both directions are covered by asserting list equality on
     *  a file that has a genuine error in it. */
    @Test(timeout = 600_000)
    fun warmAndOneShotAgreeOnTheSameBuffer() {
        val fx = fixture()
        val classpath = listOf(fx.artifact())
        val core = coreFor(fx, classpath)

        val warm = warmUnresolved(core, fx.abs(fx.bogusFile))
        val oneShot = fx.lintUnresolved(fx.bogusFile, fx.testRoot, classpath)

        assertEquals("warm and one-shot must agree", oneShot, warm)
        assertTrue(
            "...and the file's genuine error must be in both, or they agree vacuously",
            warm.contains("NoSuchTypeAtAll"),
        )
        assertTrue(
            "...while the project's own type is resolved in both",
            !warm.contains("Greeter"),
        )
        core.shutdown()
    }

    /** 5.3.3 — the warm path is warm. The number itself is recorded in the
     *  unit's commit; what is asserted here is the property that makes the
     *  daemon worth its complexity, which is robust on any machine: a warm lint
     *  costs a fraction of the one-shot that cold-compiles the stdlib every
     *  invocation. */
    @Test(timeout = 900_000)
    fun theWarmMedianIsFarBelowTheOneShot() {
        val fx = fixture()
        val classpath = listOf(fx.artifact())
        val core = coreFor(fx, classpath)

        warmUnresolved(core, fx.abs(fx.testFile))            // prime, not measured
        val warm = (1..5).map {
            val t0 = System.nanoTime()
            warmUnresolved(core, fx.abs(fx.testFile))
            (System.nanoTime() - t0) / 1_000_000
        }.sorted()[2]

        val t0 = System.nanoTime()
        fx.lintUnresolved(fx.testFile, fx.testRoot, classpath)
        val oneShot = (System.nanoTime() - t0) / 1_000_000

        println("[unit 5] warm median ${warm}ms, one-shot ${oneShot}ms")
        assertTrue(
            "the warm median (${warm}ms) must stay well under the one-shot (${oneShot}ms)",
            warm * 2 < oneShot,
        )
        core.shutdown()
    }
}
