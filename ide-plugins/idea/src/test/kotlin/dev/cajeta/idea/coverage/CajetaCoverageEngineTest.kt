package dev.cajeta.idea.coverage

import com.intellij.coverage.CoverageDataManager
import com.intellij.coverage.CoverageEngine
import com.intellij.coverage.CoverageRunner
import com.intellij.coverage.CoverageSuitesBundle
import com.intellij.coverage.DefaultCoverageFileProvider
import com.intellij.openapi.vfs.LocalFileSystem
import com.intellij.openapi.vfs.VfsUtil
import com.intellij.testFramework.fixtures.BasePlatformTestCase
import java.io.File
import java.nio.file.Files

/**
 * ide-coverage-plan Unit 3.1 — the engine as the platform sees it.
 *
 * These go through `CoverageDataManager`, not through our own objects, because
 * the point of adopting `CoverageEngine` (spec §1.4.1) is that the gutters, the
 * tool window, suite switching and suite closing are all IntelliJ's. What is
 * worth testing is that the platform accepts what we hand it — testing that
 * IntelliJ then paints would be testing IntelliJ.
 */
class CajetaCoverageEngineTest : BasePlatformTestCase() {

    private lateinit var root: File

    override fun setUp() {
        super.setUp()
        root = Files.createTempDirectory("cajeta-coverage").toFile()
        for (name in listOf("sites.tsv", "coco.profile")) {
            File(root, name).writeText(
                javaClass.getResourceAsStream("/coco/conformance/$name")!!
                    .bufferedReader().readText()
            )
        }
        // The sources the fixture's probes refer to, so path resolution has
        // something real to resolve against.
        for (rel in listOf("probe/Cond.cajeta", "probe/Helper.cajeta")) {
            val f = File(root, rel)
            f.parentFile.mkdirs()
            f.writeText((1..45).joinToString("\n") { "// line $it" } + "\n")
        }
        VfsUtil.markDirtyAndRefresh(
            false, true, true,
            LocalFileSystem.getInstance().refreshAndFindFileByIoFile(root)!!,
        )
    }

    private fun engine(): CajetaCoverageEngine =
        CoverageEngine.EP_NAME.findExtensionOrFail(CajetaCoverageEngine::class.java)

    private fun runner(): CajetaCoverageRunner =
        CoverageRunner.getInstance(CajetaCoverageRunner::class.java)

    private fun suite(name: String, profile: File = File(root, "coco.profile")) =
        engine().createCoverageSuite(
            name, project, runner(), DefaultCoverageFileProvider(profile), profile.lastModified(),
        )!!

    // --- 3.3.a  registered, and resolvable through the platform's own EPs ----

    fun testTheEngineAndRunnerAreRegisteredOnThePlatformExtensionPoints() {
        // If plugin.xml or the module dependency is wrong this is what fails,
        // rather than every behavioural test failing for an unrelated-looking
        // reason.
        assertNotNull(engine())
        assertNotNull(runner())
        assertTrue("the runner accepts our engine", runner().acceptsCoverageEngine(engine()))
        assertEquals("profile", runner().dataFileExtension)
    }

    // --- 3.1.a  the right lines in the right files ---------------------------

    fun testALoadedSuiteCarriesCoverageForTheCajetaFilesItNames() {
        val data = suite("run-1").getCoverageData(CoverageDataManager.getInstance(project))
        assertNotNull("the suite loaded", data)
        val names = data!!.classes.keys
        assertEquals(2, names.size)
        val cond = names.single { it.endsWith("probe/Cond.cajeta") }
        val cd = data.getClassData(cond)!!

        // main() ran; neverCalled() did not. Both are instrumented, so both are
        // markable — one green, one red.
        assertTrue("an executed line is covered", cd.getLineData(19)!!.hits > 0)
        assertEquals("an unexecuted but instrumented line", 0, cd.getLineData(31)!!.hits)
        // 3.1.f: a line with no probe stays unmarkable.
        assertFalse("a line with no probe is left alone", cd.containsLine(2))
    }

    fun testEditorHighlightingAppliesToCajetaFilesOnly() {
        val cajeta = myFixture.configureByText("A.cajeta", "public class A {\n}\n")
        assertTrue(engine().coverageEditorHighlightingApplicableTo(cajeta))
        val other = myFixture.configureByText("notes.txt", "hello\n")
        assertFalse(engine().coverageEditorHighlightingApplicableTo(other))
    }

    // --- 3.1.b / 3.1.c  the tool window's numbers ----------------------------

    fun testTheAnnotatorReportsPercentagesForFilesAndDirectories() {
        val bundle = CoverageSuitesBundle(suite("run-1"))
        val annotator = engine().getCoverageAnnotator(project)
        annotator.renewCoverageData(bundle, CoverageDataManager.getInstance(project))

        val condVf = LocalFileSystem.getInstance()
            .refreshAndFindFileByIoFile(File(root, "probe/Cond.cajeta"))!!
        val fileInfo = annotator.getFileCoverageInformationString(
            project, condVf, bundle, CoverageDataManager.getInstance(project))
        assertNotNull("a covered file reports a percentage", fileInfo)
        assertTrue("reads as a percentage: $fileInfo", fileInfo!!.contains("%"))

        val dirInfo = annotator.getDirCoverageInformationString(
            project, condVf.parent, bundle, CoverageDataManager.getInstance(project))
        assertNotNull("its directory rolls up", dirInfo)
    }

    fun testLineBranchAndFunctionMetricsAreAllAvailable() {
        val bundle = CoverageSuitesBundle(suite("run-1"))
        val annotator = engine().getCoverageAnnotator(project) as CajetaCoverageAnnotator
        annotator.renewCoverageData(bundle, CoverageDataManager.getInstance(project))
        val condVf = LocalFileSystem.getInstance()
            .refreshAndFindFileByIoFile(File(root, "probe/Cond.cajeta"))!!

        val info = annotator.metricsFor(condVf)!!
        assertTrue("lines counted", info.totalLines > 0)
        assertTrue("some lines covered", info.coveredLines in 1 until info.totalLines)
        assertEquals("Cond's 21 instrumented lines", 21, info.totalLines)
        assertEquals(14, info.coveredLines)
        assertEquals("Cond's 7 decisions, two arms each", 14, info.totalBranches)
        assertEquals(7, info.coveredBranches)
        assertEquals("Cond's six methods", 6, info.totalFunctions)
        assertEquals("all but guarded() and neverCalled()", 4, info.coveredFunctions)
    }

    fun testAFileWithNoCoverageDataIsNotReportedAsZero() {
        // 3.1.f. Reporting 0% for a file coco never saw is a false negative that
        // reads exactly like a real one.
        val bundle = CoverageSuitesBundle(suite("run-1"))
        val annotator = engine().getCoverageAnnotator(project)
        annotator.renewCoverageData(bundle, CoverageDataManager.getInstance(project))

        val stranger = File(root, "probe/Untouched.cajeta").also { it.writeText("// nothing\n") }
        val vf = LocalFileSystem.getInstance().refreshAndFindFileByIoFile(stranger)!!
        assertNull(
            annotator.getFileCoverageInformationString(
                project, vf, bundle, CoverageDataManager.getInstance(project)),
        )
    }

    // --- 3.1.d  closing a suite removes every marking ------------------------

    fun testClosingTheSuiteClearsTheCurrentBundle() {
        // Goes through the platform's own import path — addExternalCoverageSuite
        // is what the Import Coverage action calls — so this exercises the real
        // wiring rather than a hand-built suite.
        val manager = CoverageDataManager.getInstance(project)
        val s = manager.addExternalCoverageSuite(File(root, "coco.profile"), runner())
        val bundle = CoverageSuitesBundle(s)
        manager.chooseSuitesBundle(bundle)
        assertNotNull(manager.currentSuitesBundle)

        manager.closeSuitesBundle(bundle)
        assertNull("no bundle means no annotations anywhere", manager.currentSuitesBundle)
    }

    // --- 3.1.e  two suites, switchable ---------------------------------------

    fun testTwoSuitesCanBeLoadedAndSelectedBetween() {
        val manager = CoverageDataManager.getInstance(project)
        val second = File(root, "run-2.profile")
        second.writeText(File(root, "coco.profile").readText())
        val first = File(root, "run-1.profile")
        first.writeText(File(root, "coco.profile").readText())

        val a = manager.addExternalCoverageSuite(first, runner())
        val b = manager.addExternalCoverageSuite(second, runner())

        // Filter to THESE two files rather than counting every Cajeta suite in
        // the project: suites accumulate by design, and the light fixture shares
        // one project across test methods, so a global count silently depends on
        // what else ran first.
        val ours = manager.suites.filter {
            it.runner is CajetaCoverageRunner &&
                it.coverageDataFileName in setOf(first.absolutePath, second.absolutePath)
        }
        assertEquals(2, ours.size)
        assertEquals(
            setOf("run-1.profile", "run-2.profile"),
            ours.map { it.presentableName }.toSet(),
        )

        val bundleA = CoverageSuitesBundle(a)
        manager.chooseSuitesBundle(bundleA)
        assertEquals("run-1.profile", manager.currentSuitesBundle!!.suites.single().presentableName)
        val bundleB = CoverageSuitesBundle(b)
        manager.chooseSuitesBundle(bundleB)
        assertEquals("run-2.profile", manager.currentSuitesBundle!!.suites.single().presentableName)

        manager.closeSuitesBundle(bundleB)
    }

    fun testAProfileWithNoSiteTableLoadsNothingRatherThanGuessing() {
        // Probe ids are positional against the site table, so a profile read
        // without its own table attributes hits to whatever table turned up.
        val orphan = Files.createTempDirectory("coco-orphan").toFile()
        val profile = File(orphan, "coco.profile")
        profile.writeText(File(root, "coco.profile").readText())
        val s = suite("orphan", profile)
        assertNull(s.getCoverageData(CoverageDataManager.getInstance(project)))
    }
}
