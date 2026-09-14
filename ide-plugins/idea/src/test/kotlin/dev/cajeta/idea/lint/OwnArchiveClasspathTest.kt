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

    /** 6.1.1 / 6.1.2 — the whole-root xref export is what the shard is built
     *  from, and therefore what Ctrl-click resolves against. With the own
     *  archive on its classpath the export carries the project's own symbols,
     *  so an import in a test-root file has a target.
     *
     *  This is the second reported symptom ("the imports are not clickable"),
     *  and it is NOT fixed by the one-shot lint path in Unit 3 — the export is
     *  a separate invocation with its own classpath. */
    @Test
    fun theWholeRootExportCarriesTheProjectsOwnSymbols() {
        val fx = twoRootFixture()
        val json = fx.exportXref(fx.testRoot, classpath = listOf(fx.artifact()))
        assertTrue(
            "the export must name the project's own type so Ctrl-click has a target",
            json.contains("Greeter"),
        )
    }

    /** 7.1.8 — the end-to-end check that would have caught the coverage gap.
     *
     *  Unit 6 made the export able to RESOLVE the project's own types; it did
     *  not make the export VISIT the test root. Exporting only the conventional
     *  root leaves the test file absent from the index, so every import in it is
     *  dead — measured on cajeta-http, the shard named `HttpSerializer` 587 times
     *  and `ServerTests` 0. Discovering roots and exporting each is what fixes
     *  it, and this asserts the test class actually lands in a document. */
    @Test
    fun exportingEveryDiscoveredRootReachesTheTestClass() {
        val fx = twoRootFixture()
        val roots = dev.cajeta.idea.buildtool.CajetaRoots.sourceRootsOf(fx.root.toString())
        assertTrue("both source roots must be discovered; got $roots", roots.size >= 2)
        val combined = roots.joinToString("\n") { fx.exportXref(it, classpath = listOf(fx.artifact())) }
        assertTrue(
            "the exported documents must name the TEST class, not only the library class",
            combined.contains("UseGreeter"),
        )
        assertTrue(
            "...and still name the library class",
            combined.contains("Greeter"),
        )
    }

    /** The does-not-fire control for 7.1.8: exporting ONLY the conventional root
     *  reproduces the bug — the test class is absent. Without this, 7.1.8 could
     *  pass for the wrong reason. */
    @Test
    fun exportingOnlyTheConventionalRootMissesTheTestClass() {
        val fx = twoRootFixture()
        val conventional = dev.cajeta.idea.buildtool.CajetaRoots
            .conventionalSourceRoot(fx.root.toString())
        val json = fx.exportXref(conventional, classpath = listOf(fx.artifact()))
        assertTrue(
            "the conventional root alone cannot reach the test class — that is the bug",
            !json.contains("UseGreeter"),
        )
    }

    /** 4.1.1 / 4.3.1 / 4.3.2 — the states against a REAL project, not just the
     *  pure assessor. A built project is silent; the same project with its
     *  archive removed reports ABSENT and still lints (no build is triggered). */
    @Test
    fun aBuiltProjectIsSilentAndAnUnbuiltOneReportsAbsent() {
        val fx = twoRootFixture()
        val glue = dev.cajeta.idea.xref.CajetaSourceMountGlue
        assertEquals(
            "a built project must say nothing",
            dev.cajeta.idea.xref.ArchiveHealth.State.OK,
            glue.archiveHealth(fx.compilerPath(), fx.root.toString()),
        )

        val art = fx.artifact()
        val hidden = java.nio.file.Path.of("$art.hidden")
        java.nio.file.Files.move(art, hidden)
        try {
            assertEquals(
                "with the archive gone, the reason must be reported",
                dev.cajeta.idea.xref.ArchiveHealth.State.ABSENT,
                glue.archiveHealth(fx.compilerPath(), fx.root.toString()),
            )
            assertTrue(
                "and lint still runs rather than failing or building",
                fx.lintUnresolved(fx.testFile, fx.testRoot, classpath = emptyList()).isNotEmpty(),
            )
            assertTrue("no build may be triggered", !java.nio.file.Files.exists(art))
        } finally {
            java.nio.file.Files.move(hidden, art)
        }
    }

    /** 6.1.4 — the does-not-fire control. Without the own archive the export is
     *  empty of the project's own symbols; that is the pre-existing behaviour
     *  this unit changes, and stating it keeps 6.1.2 from being vacuous. */
    @Test
    fun withoutTheOwnArchiveTheExportHasNoneOfThoseSymbols() {
        val fx = twoRootFixture()
        val json = fx.exportXref(fx.testRoot, classpath = emptyList())
        assertTrue(
            "without the archive the export cannot name the project's own type; got ${json.length} bytes",
            !json.contains("Greeter"),
        )
    }

    /** 6.1.3 — the shard-clobber hole (spec §6.5).
     *
     *  The per-edit lint stream OVERWRITES the whole-root export's shard for the
     *  file being edited (`CajetaXrefShards.ingestStream`), so whatever that
     *  stream fails to resolve is not merely missing from it — it is erased from
     *  the index Unit 6 just populated. The same mechanism is already documented
     *  for dependency targets at `CajetaLintAnnotator.kt:20-24`; the project's own
     *  types are the case that was left out.
     *
     *  Asserted against the real shard TEXT, which is what lands on disk, rather
     *  than against the record list — a record the shard writer drops would pass
     *  a record-level check and still break Ctrl-click. */
    @Test
    fun aPerEditLintStreamKeepsTheProjectsOwnTargets() {
        val fx = twoRootFixture()

        // The FULLY QUALIFIED name, never the bare one. The test class is called
        // `UseGreeter`, so `contains("Greeter")` is true of a shard carrying only
        // the file's OWN declarations — which is exactly the clobbered state this
        // is supposed to detect. Written that way first, and the does-not-fire
        // half below is what caught it.
        val own = "com.example.library.Greeter"

        fun shardFor(classpath: List<java.nio.file.Path>): String {
            val argv = CajetacRunner.lintArgv(
                fx.compilerPath(), fx.abs(fx.testFile), fx.abs(fx.testRoot),
                shadow = null, emitXref = true, classpath = classpath,
            )
            val stream = XrefStreamParser.demux(fx.outputOf(argv))
            assertTrue("the stream must be readable, or this proves nothing", stream.supported)
            assertTrue(
                "the stream must carry this file's own declarations either way, " +
                "or the comparison below is between two empty shards",
                stream.records.any { it.rel == "declarations" },
            )
            return dev.cajeta.idea.xref.CajetaXrefShards.shardText(stream.records)
        }

        assertTrue(
            "the per-edit stream that overwrites the shard must still carry the " +
            "project's own targets",
            shardFor(listOf(fx.artifact())).contains(own),
        )
        assertTrue(
            "...and without the archive it does not — that is the clobber this " +
            "unit closes, and stating it keeps the assertion above from being vacuous",
            !shardFor(emptyList()).contains(own),
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
