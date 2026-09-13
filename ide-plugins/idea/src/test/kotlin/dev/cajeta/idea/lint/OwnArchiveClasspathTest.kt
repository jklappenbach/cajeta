package dev.cajeta.idea.lint

import org.junit.AfterClass
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.BeforeClass
import org.junit.Test

/**
 * lint-own-archive-classpath Unit 1 — the check that would have caught it.
 *
 * The project's own build artifact is not on the lint classpath
 * (`CajetaSourceMountGlue.dependencyArchives` scans only the dependency cache),
 * so a file in a *test* source root cannot resolve its own project's types even
 * though the build resolves them fine. `run-tests.sh` compiles such a suite as
 * one source root plus the project's own `.cja`; lint passes the root and omits
 * the archive.
 *
 * These tests pin the behaviour at the CLI level BEFORE the fix, so Unit 3 is
 * provable and a regression is visible. [twoRootWithoutOwnArchiveReportsUnresolved]
 * asserts the bug is present; it flips to the fixed expectation in Unit 3.
 *
 * The fixture is built from the toolchain's own `library` archetype in a temp
 * dir — nothing here depends on a checkout of `cajeta-http` (plan 1.3.3).
 * Skipped wholesale when no compiler is configured (`CAJETA_TEST_COMPILER`).
 */
class OwnArchiveClasspathTest {

    companion object {
        private var twoRoot: LintFixture? = null
        private var singleRoot: LintFixture? = null

        @BeforeClass
        @JvmStatic
        fun build() {
            if (!LintFixture.compilerAvailable()) return
            twoRoot = LintFixture.twoRoot()
            singleRoot = LintFixture.singleRoot()
        }

        @AfterClass
        @JvmStatic
        fun cleanUp() {
            twoRoot?.delete()
            singleRoot?.delete()
        }
    }

    private fun twoRootFixture(): LintFixture {
        assumeTrue("no compiler configured (CAJETA_TEST_COMPILER)", LintFixture.compilerAvailable())
        return twoRoot!!
    }

    private fun singleRootFixture(): LintFixture {
        assumeTrue("no compiler configured (CAJETA_TEST_COMPILER)", LintFixture.compilerAvailable())
        return singleRoot!!
    }

    /** 1.1.1 — asserts the BUG. A test-root file cannot see its own project's
     *  types when the own archive is absent from the classpath. Unit 3 flips
     *  this expectation to zero; until then it documents the defect. */
    @Test
    fun twoRootWithoutOwnArchiveReportsUnresolved() {
        val fx = twoRootFixture()
        val unresolved = fx.lintUnresolved(fx.testFile, fx.testRoot, classpath = emptyList())
        assertTrue(
            "the project's own type must be unresolved without its archive on the classpath; got $unresolved",
            unresolved.contains("Greeter"),
        )
    }

    /** 1.1.2 — the same lint with the own archive resolves cleanly. */
    @Test
    fun twoRootWithOwnArchiveResolvesCleanly() {
        val fx = twoRootFixture()
        val unresolved = fx.lintUnresolved(fx.testFile, fx.testRoot, classpath = listOf(fx.artifact()))
        assertEquals("the own archive must resolve the project's own types", emptyList<String>(), unresolved)
    }

    /** 1.1.3 — the control. A single-root project never had this problem, and
     *  adding the archive must not change that (plan 1.3.3 / spec §7.4). */
    @Test
    fun singleRootResolvesWithAndWithoutTheArchive() {
        val fx = singleRootFixture()
        assertEquals(
            "a same-root sibling resolves without any archive",
            emptyList<String>(),
            fx.lintUnresolved(fx.testFile, fx.mainRoot, classpath = emptyList()),
        )
        assertEquals(
            "and adding the archive changes nothing",
            emptyList<String>(),
            fx.lintUnresolved(fx.testFile, fx.mainRoot, classpath = listOf(fx.artifact())),
        )
    }

    /** 3.1.4 — the red→green test for the fix. Builds the argv the way the
     *  PLUGIN builds it (source root from `sourceRootOf`, classpath from the
     *  production discovery in Unit 2) and asserts the project's own types
     *  resolve. Fails before Unit 3's wiring, passes after. */
    @Test
    fun thePluginsOwnArgvResolvesTheProjectsOwnTypes() {
        val fx = twoRootFixture()
        val file = fx.abs(fx.testFile)
        val sourceRoot = CajetacRunner.sourceRootOf(file, java.io.File(file).readText())
        val classpath = dev.cajeta.idea.xref.CajetaSourceMountGlue
            .lintClasspath(fx.compilerPath(), fx.root.toString())
        val argv = CajetacRunner.lintArgv(
            fx.compilerPath(), file, sourceRoot, null, emitXref = false, classpath = classpath,
        )
        assertTrue(
            "the production classpath must carry the project's own archive; got $classpath",
            classpath.any { it.toString().endsWith(".cja") },
        )
        assertEquals(
            "the plugin's own argv must resolve the project's own types",
            emptyList<String>(), fx.unresolvedFrom(argv),
        )
    }

    /** 1.1.4 — the does-not-fire case. With the archive present a genuinely
     *  undefined type is STILL reported, so a clean run cannot be mistaken for
     *  suppressed diagnostics. */
    @Test
    fun anUndefinedTypeIsStillReportedWithTheArchive() {
        val fx = twoRootFixture()
        val unresolved = fx.lintUnresolved(fx.bogusFile, fx.testRoot, classpath = listOf(fx.artifact()))
        assertTrue(
            "a genuinely undefined type must still be reported; got $unresolved",
            unresolved.contains("NoSuchTypeAtAll"),
        )
        assertTrue(
            "...and the project's own type must not be, or the check is vacuous; got $unresolved",
            !unresolved.contains("Greeter"),
        )
    }
}
