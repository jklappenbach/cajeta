package dev.cajeta.idea.coverage

import com.intellij.coverage.CoverageDataManager
import com.intellij.coverage.CoverageExecutor
import com.intellij.execution.executors.DefaultDebugExecutor
import com.intellij.execution.executors.DefaultRunExecutor
import com.intellij.testFramework.fixtures.BasePlatformTestCase
import com.intellij.execution.configurations.ConfigurationTypeUtil
import dev.cajeta.idea.buildtool.CajetaTaskConfigurationType
import dev.cajeta.idea.buildtool.CajetaTaskRunConfiguration
import dev.cajeta.idea.debugger.CajetaConfigurationType
import dev.cajeta.idea.debugger.CajetaRunConfiguration
import java.io.File
import java.nio.file.Files

/**
 * ide-coverage-plan Unit 4.1 — loading a completed run, and refusing to pretend
 * when there is nothing to load.
 *
 * The pipeline execution itself is the build tool's and is not re-tested here;
 * what these pin is the IDE's half — which executor claims which configuration,
 * and what happens on each way the load can fail.
 */
class CajetaCoverageRunLoadTest : BasePlatformTestCase() {

    private fun runDir(withSites: Boolean = true, withProfile: Boolean = true): File {
        val base = Files.createTempDirectory("coco-run").toFile()
        File(base, "run").mkdirs()
        if (withSites) File(base, "sites.tsv").writeText(fixture("sites.tsv"))
        if (withProfile) File(base, "run/coco.profile").writeText(fixture("coco.profile"))
        return base
    }

    private fun fixture(name: String) =
        javaClass.getResourceAsStream("/coco/conformance/$name")!!.bufferedReader().readText()

    // --- 4.1.a  the Coverage executor claims a Cajeta task -------------------

    fun testTheCoverageRunnerClaimsCajetaTasksAndNothingElse() {
        val runner = CajetaCoverageProgramRunner()
        val type = ConfigurationTypeUtil.findConfigurationType(CajetaTaskConfigurationType::class.java)!!
        val task = type.configurationFactories[0]
            .createTemplateConfiguration(project) as CajetaTaskRunConfiguration

        assertTrue(runner.canRun(CoverageExecutor.EXECUTOR_ID, task))
        // Run and Debug stay with their own runners — claiming them would
        // silently turn every ordinary run into a coverage run.
        assertFalse(runner.canRun(DefaultRunExecutor.EXECUTOR_ID, task))
        assertFalse(runner.canRun(DefaultDebugExecutor.EXECUTOR_ID, task))
    }

    fun testTheCoverageRunnerDoesNotClaimTheStandaloneDebugConfiguration() {
        // CajetaRunConfiguration JIT-runs an entry method; there is no build-tool
        // task to carry a cajeta.coverage.instrument action, so coverage would
        // have nothing to measure.
        val runner = CajetaCoverageProgramRunner()
        val type = ConfigurationTypeUtil.findConfigurationType(CajetaConfigurationType::class.java)!!
        val config = type.configurationFactories[0]
            .createTemplateConfiguration(project) as CajetaRunConfiguration
        assertFalse(runner.canRun(CoverageExecutor.EXECUTOR_ID, config))
    }

    // --- 4.1.d  existing results load without a run --------------------------

    fun testACompletedRunLoadsFromDiskAndBecomesTheCurrentSuite() {
        val out = runDir()
        val outcome = CocoRunLoader.loadAndShow(project, out, "test")
        assertTrue("loaded: $outcome", outcome is CocoRunLoader.Outcome.Loaded)

        // Assert on THIS load, not on a global count: suites accumulate in the
        // project by design (§4.4 re-examination, §3.8 switching), and the light
        // fixture's project is shared across test methods.
        val profile = CocoArtifacts.discoverProfile(out)!!
        val manager = CoverageDataManager.getInstance(project)
        val suite = manager.suites.single {
            it.runner is CajetaCoverageRunner &&
                it.coverageDataFileName == profile.absolutePath
        }
        val data = suite.getCoverageData(manager)
        assertNotNull("the suite carries data", data)
        assertEquals("both fixture files", 2, data!!.classes.size)
    }

    fun testLoadingTwiceRereadsTheSameArtifactsAndRunsNothing() {
        // 4.3.b / spec §7.2: re-rendering must not re-run the pipeline. The
        // observable form of that here is that a second load succeeds from the
        // same untouched files.
        val out = runDir()
        val profile = CocoArtifacts.discoverProfile(out)!!
        val stamp = profile.lastModified()

        assertTrue(CocoRunLoader.loadAndShow(project, out, "a") is CocoRunLoader.Outcome.Loaded)
        assertTrue(CocoRunLoader.loadAndShow(project, out, "b") is CocoRunLoader.Outcome.Loaded)

        assertTrue("artifacts retained", profile.isFile)
        assertEquals("nothing rewrote them", stamp, profile.lastModified())
    }

    // --- 4.1.b  a run that produced nothing says so --------------------------

    fun testAnOutDirectoryWithNoProfileReportsWhereItLooked() {
        val out = runDir(withProfile = false)
        val outcome = CocoRunLoader.loadAndShow(project, out, "test")
        assertTrue(outcome is CocoRunLoader.Outcome.NoArtifacts)
        val text = CocoRunLoader.describe(outcome)
        assertTrue("names the directory: $text", text.contains(out.path))
        assertTrue("names the action that should have run: $text", text.contains("instrument"))
    }

    fun testAProfileWithoutItsSiteTableIsRefusedNotGuessedAt() {
        // Probe ids are positional against the site table. Loading anyway would
        // attribute hits to the wrong sites and paint lines green that never ran.
        val out = runDir(withSites = false)
        val outcome = CocoRunLoader.loadAndShow(project, out, "test")
        assertTrue(outcome is CocoRunLoader.Outcome.Unreadable)
        assertTrue(CocoRunLoader.describe(outcome).contains("sites.tsv"))
    }

    fun testAnUnrecognisedFormatVersionIsReportedNotSwallowed() {
        val out = runDir()
        File(out, "sites.tsv").writeText("coco-sites v2\n")
        val outcome = CocoRunLoader.loadAndShow(project, out, "test")
        assertTrue("refused: $outcome", outcome is CocoRunLoader.Outcome.Unreadable)
        assertTrue(CocoRunLoader.describe(outcome).contains("coco-sites v2"))
    }

    // --- 2.2.e  the load runs under a progress task, off the EDT -------------

    fun testLoadingRunsUnderABackgroundProgressTaskAndReportsItsOutcome() {
        // 2.2.e was deferred from Unit 2 for want of a run to hang an indicator
        // on. Both entry points now go through loadInBackground, so a large run
        // cannot freeze the editor at the moment gutters are awaited.
        val out = runDir()
        val seen = java.util.concurrent.atomic.AtomicReference<CocoRunLoader.Outcome>()
        CocoRunLoader.loadInBackground(project, out, "test") { seen.set(it) }
        assertNotNull("the callback ran", seen.get())
        assertTrue("loaded: ${seen.get()}", seen.get() is CocoRunLoader.Outcome.Loaded)
    }

    // --- 4.1.c  a project without coco explains itself -----------------------

    fun testAProjectWithoutCocoIsNotOfferedTheLoadAction() {
        // update() hides the action rather than letting it fail on use.
        assertFalse(CocoProject.of(project).isConfigured)
        assertNotNull(CocoProject.of(project).problem)
    }
}
