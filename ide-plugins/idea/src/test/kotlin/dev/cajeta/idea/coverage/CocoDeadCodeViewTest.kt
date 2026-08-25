package dev.cajeta.idea.coverage

import com.intellij.testFramework.fixtures.BasePlatformTestCase
import dev.cajeta.idea.xref.CajetaXrefFreshness
import java.io.File
import java.nio.file.Files

/**
 * ide-coverage-plan Unit 6.1.d and the view's own reporting.
 *
 * The panel is Swing and mostly IntelliJ's; what is worth testing is what it
 * SAYS and where it goes — the summary line and the navigation target — both of
 * which are decisions, not rendering.
 */
class CocoDeadCodeViewTest : BasePlatformTestCase() {

    private fun finding(
        method: String,
        verdict: Verdict,
        file: String = "probe/Cond.cajeta",
        line: Int = 10,
    ) = UncoveredMethod(
        key = "probe.Cond::${method.substringBefore('(')}/1",
        owner = "probe.Cond", method = method, file = file, line = line,
        verdict = verdict, reason = "because",
    )

    // --- the summary states every bucket, including what it does not know ----

    fun testTheSummaryNamesAllThreeBucketsSoNothingIsQuietlyDropped() {
        val text = CocoDeadCodePanel.summarize(
            listOf(
                finding("a()", Verdict.NEEDS_A_TEST),
                finding("b()", Verdict.DELETION_CANDIDATE),
                finding("c()", Verdict.UNDETERMINED),
            )
        )
        assertTrue(text, text.contains("1 need a test"))
        assertTrue(text, text.contains("1 unreachable"))
        // Counting only the two confident buckets would overstate how much the
        // analysis actually knows.
        assertTrue("undetermined is reported, not hidden: $text", text.contains("1 undetermined"))
        assertTrue(text, text.contains("3 uncovered"))
    }

    fun testTheSummaryOmitsTheUndeterminedClauseWhenThereIsNone() {
        val text = CocoDeadCodePanel.summarize(listOf(finding("a()", Verdict.NEEDS_A_TEST)))
        assertFalse(text, text.contains("undetermined"))
    }

    fun testNothingUncoveredSaysSoRatherThanShowingAnEmptyList() {
        assertTrue(CocoDeadCodePanel.summarize(emptyList()).contains("Nothing uncovered"))
    }

    fun testTheTwoVerdictsAreLabelledAsDifferentKindsOfWork() {
        // An lcov report renders these as identical red; the whole unit exists
        // to distinguish work-to-add from work-to-remove.
        assertFalse(
            CocoDeadCodePanel.labelOf(Verdict.DELETION_CANDIDATE) ==
                CocoDeadCodePanel.labelOf(Verdict.NEEDS_A_TEST)
        )
        assertTrue(CocoDeadCodePanel.labelOf(Verdict.UNDETERMINED).contains("UNKNOWN"))
    }

    /**
     * A label names the measured condition; it does not issue an instruction,
     * and it does not carry its uncertainty in punctuation.
     *
     * These read `DELETE?` and `TEST`. Analysis tooling does not label findings
     * that way — Rust "never used", Go "is unused", IntelliJ "is never used"
     * with Safe Delete offered separately as the action — and coco's own
     * Mutants tab already follows the convention (`KILLED`/`SURVIVED`). The
     * recommendation belongs on the context menu, not in the row.
     */
    fun testLabelsNameTheConditionRatherThanCommandingAnAction() {
        for (v in Verdict.entries) {
            val label = CocoDeadCodePanel.labelOf(v)
            assertFalse(
                "a label hedged with punctuation reads as a glitch; say what " +
                    "was measured instead: $label",
                label.contains("?"),
            )
            assertFalse(
                "'$label' is an instruction; the row reports, the context menu " +
                    "recommends",
                label == "DELETE" || label == "TEST" || label == "FIX",
            )
        }
        assertEquals("UNREACHABLE", CocoDeadCodePanel.labelOf(Verdict.DELETION_CANDIDATE))
        assertEquals("UNTESTED", CocoDeadCodePanel.labelOf(Verdict.NEEDS_A_TEST))
    }

    // --- 6.1.d  navigation is one action, and resolves a real file -----------

    fun testNavigationResolvesACocoRelativePathAgainstTheProject() {
        val dir = Files.createTempDirectory("coco-nav").toFile()
        val src = File(dir, "probe/Cond.cajeta")
        src.parentFile.mkdirs()
        src.writeText((1..20).joinToString("\n") { "// line $it" } + "\n")

        // Absolute paths resolve directly.
        assertNotNull(CocoNavigation.resolve(project, src.absolutePath))
        // A path naming nothing resolves to nothing rather than to some other
        // file that happens to share a name.
        assertNull(CocoNavigation.resolve(project, "no/such/File.cajeta"))
    }

    // --- 6.1.c  the freshness gate ------------------------------------------

    fun testReachabilityIsRefusedWholesaleOnAStaleIndex() {
        val freshness = CajetaXrefFreshness.getInstance(project)
        freshness.refreshFailed("export never ingested")
        val reason = CocoXrefEdges.unavailableReason(project)
        assertNotNull("a stale index must not be used for reachability", reason)
        assertTrue(reason!!, reason.contains("not up to date"))
        assertTrue("says what it refuses to do: $reason", reason.contains("unreachable"))

        freshness.refreshSucceeded()
        assertNull("a fresh index is usable", CocoXrefEdges.unavailableReason(project))
    }

    fun testAnIndexStillRefreshingIsNotTreatedAsEvidenceOfDeath() {
        val freshness = CajetaXrefFreshness.getInstance(project)
        freshness.refreshStarted()
        val reason = CocoXrefEdges.unavailableReason(project)
        assertNotNull(reason)
        assertTrue(reason!!, reason.contains("refreshing"))
        freshness.refreshSucceeded()
    }
}
