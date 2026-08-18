package dev.cajeta.idea.coverage

import com.intellij.coverage.CoverageDataManager
import com.intellij.coverage.CoverageEngine
import com.intellij.coverage.CoverageSuitesBundle
import com.intellij.openapi.vfs.LocalFileSystem
import com.intellij.testFramework.fixtures.BasePlatformTestCase
import java.io.File
import java.nio.file.Files

/**
 * ide-coverage-plan Unit 5.1.b / 5.3.a — staleness as the platform sees it.
 *
 * The acceptance criterion for this unit is a negative: coverage is NEVER drawn
 * against source it was not measured on. The observable form of that is the
 * engine declining to highlight a stale file, which is what these check.
 */
class CocoStalenessIntegrationTest : BasePlatformTestCase() {

    private lateinit var root: File

    private fun loadRun(): File {
        root = Files.createTempDirectory("coco-stale").toFile()
        File(root, "run").mkdirs()
        for ((rel, res) in listOf("sites.tsv" to "sites.tsv", "run/coco.profile" to "coco.profile")) {
            File(root, rel).writeText(
                javaClass.getResourceAsStream("/coco/conformance/$res")!!.bufferedReader().readText()
            )
        }
        for (rel in listOf("probe/Cond.cajeta", "probe/Helper.cajeta")) {
            val f = File(root, rel)
            f.parentFile.mkdirs()
            f.writeText((1..45).joinToString("\n") { "// line $it" } + "\n")
            f.setLastModified(File(root, "run/coco.profile").lastModified() - 10_000)
        }
        val outcome = CocoRunLoader.loadAndShow(project, root, "stale-test")
        assertTrue("loaded: $outcome", outcome is CocoRunLoader.Outcome.Loaded)
        return root
    }

    private fun engine(): CajetaCoverageEngine =
        CoverageEngine.EP_NAME.findExtensionOrFail(CajetaCoverageEngine::class.java)

    private fun psiFor(io: File) = com.intellij.psi.PsiManager.getInstance(project)
        .findFile(LocalFileSystem.getInstance().refreshAndFindFileByIoFile(io)!!)!!

    override fun tearDown() {
        try { CocoFreshness.getInstance(project).clear() } finally { super.tearDown() }
    }

    fun testAFreshFileIsHighlightedAndAnEditedOneIsNot() {
        loadRun()
        val cond = File(root, "probe/Cond.cajeta")
        val helper = File(root, "probe/Helper.cajeta")

        assertTrue("fresh files are highlighted", engine().coverageEditorHighlightingApplicableTo(psiFor(cond)))

        // Edit Cond: every marking below the insertion now points at the wrong
        // line, so none may be drawn.
        cond.writeText("// inserted\n" + cond.readText())
        cond.setLastModified(cond.lastModified() + 60_000)

        assertFalse(
            "an edited file must not be drawn against shifted lines",
            engine().coverageEditorHighlightingApplicableTo(psiFor(cond)),
        )
        // 5.1.c: its neighbour is untouched and still true.
        assertTrue(
            "an unchanged neighbour keeps its markings",
            engine().coverageEditorHighlightingApplicableTo(psiFor(helper)),
        )
    }

    fun testAStaleFileReportsStalenessInsteadOfAPercentage() {
        loadRun()
        // Select by data file, not by position: suites accumulate in the shared
        // light project and their order is not insertion order.
        val profile = CocoArtifacts.discoverProfile(root)!!
        val bundle = CoverageSuitesBundle(
            CoverageDataManager.getInstance(project).suites.single {
                it.runner is CajetaCoverageRunner &&
                    it.coverageDataFileName == profile.absolutePath
            }
        )
        val annotator = engine().getCoverageAnnotator(project)
        annotator.renewCoverageData(bundle, CoverageDataManager.getInstance(project))

        val cond = File(root, "probe/Cond.cajeta")
        val vf = LocalFileSystem.getInstance().refreshAndFindFileByIoFile(cond)!!
        val fresh = annotator.getFileCoverageInformationString(
            project, vf, bundle, CoverageDataManager.getInstance(project))
        assertNotNull(fresh)
        assertTrue("a fresh file shows a percentage: $fresh", fresh!!.contains("%"))

        cond.writeText("// changed\n")
        cond.setLastModified(cond.lastModified() + 60_000)
        val stale = annotator.getFileCoverageInformationString(
            project, vf, bundle, CoverageDataManager.getInstance(project))
        // A precise, plausible, no-longer-true number is the failure this unit
        // exists to prevent.
        assertFalse("no percentage once stale: $stale", stale!!.contains("%"))
        assertTrue("says stale: $stale", stale.contains("stale"))
    }

    fun testTheStaleNotificationNamesTheFilesAndSaysWhatItDid() {
        val text = CocoStaleNotifier.message(listOf("/p/A.cajeta", "/p/B.cajeta"))
        assertTrue(text.contains("A.cajeta"))
        assertTrue(text.contains("B.cajeta"))
        assertTrue("says markings are hidden, not silently wrong: $text", text.contains("hidden"))
        assertTrue("plural agreement: $text", text.contains("have changed"))
        assertTrue(CocoStaleNotifier.message(listOf("/p/A.cajeta")).contains("has changed"))
    }

    fun testALongStaleListIsSummarisedRatherThanDumped() {
        val many = (1..9).map { "/p/F$it.cajeta" }
        val text = CocoStaleNotifier.message(many)
        assertTrue("names a few: $text", text.contains("F1.cajeta"))
        assertTrue("counts the rest: $text", text.contains("6 more"))
    }

    fun testNothingIsReportedWhenNothingIsStale() {
        loadRun()
        assertNull(CocoStaleNotifier.notifyIfStale(project))
    }

    // --- 5.2.c  the re-run offer ---------------------------------------------

    fun testTheRerunOfferIsWithheldWhenThereIsNoRunToRepeat() {
        // Offering "Re-run" with nothing to re-run would be a dead button. It is
        // withheld rather than shown-and-failing.
        assertFalse(CocoLastRun.getInstance(project).canRerun())
        assertFalse("nothing to restart", CocoLastRun.getInstance(project).rerun())
    }

    fun testTheStaleWarningIsRaisedOnceNotOncePerSave() {
        // A warning repeated on every autosave is one people learn to dismiss
        // without reading, which costs more than saying nothing.
        loadRun()
        val cond = File(root, "probe/Cond.cajeta")
        cond.writeText("// changed\n")
        cond.setLastModified(cond.lastModified() + 60_000)

        val watcher = CocoStalenessWatcher()
        val first = watcher.staleSetIfNewlyAnnounced(project)
        val second = watcher.staleSetIfNewlyAnnounced(project)
        assertNotNull("announced the first time", first)
        assertNull("silent the second time, nothing having changed", second)

        // A NEW file going stale is new information and is announced again.
        val helper = File(root, "probe/Helper.cajeta")
        helper.writeText("// changed too\n")
        helper.setLastModified(helper.lastModified() + 60_000)
        assertNotNull("a newly stale file is new information", watcher.staleSetIfNewlyAnnounced(project))
    }
}
