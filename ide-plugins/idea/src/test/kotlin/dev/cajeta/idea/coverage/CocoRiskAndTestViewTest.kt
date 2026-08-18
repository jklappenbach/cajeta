package dev.cajeta.idea.coverage

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ide-coverage-plan Unit 7.2 — what the two new tabs SAY. The rendering is
 * Swing's; the wording is a decision.
 */
class CocoRiskAndTestViewTest {

    private fun entry(score: Int) = CrapEntry("demo.A.b()", 3, 500, score)

    @Test
    fun theRiskSummaryCountsWhatIsAboveTheThreshold() {
        val text = CocoRiskPanel.summarize(listOf(entry(1183), entry(295), entry(10)))
        assertTrue(text, text.contains("3 methods ranked"))
        assertTrue("names the threshold so the count is interpretable: $text",
            text.contains("1 above the CRAP threshold of 30"))
    }

    @Test
    fun anEmptyRankingSaysSoRatherThanReadingAsZeroRisk() {
        assertTrue(CocoRiskPanel.summarize(emptyList()).contains("No methods ranked"))
    }

    // --- redundancy is a candidate, never a verdict --------------------------

    @Test
    fun aTestWithNoUniqueCoverageIsCalledACandidateNotDeletable() {
        val text = CocoTestImpactPanel.describe(CocoTestSummary("Beta", covered = 12, unique = 0))
        assertTrue(text, text.contains("redundancy candidate"))
        // A test can be worth keeping for what it ASSERTS even when another
        // test happens to execute the same lines. Coverage overlap is evidence,
        // not proof, so the view must not say "delete".
        assertFalse("must not instruct deletion: $text", text.contains("delete"))
    }

    @Test
    fun aTestWithUniqueCoverageReportsHowMuch() {
        val text = CocoTestImpactPanel.describe(CocoTestSummary("Alpha", covered = 12, unique = 4))
        assertTrue(text, text.contains("4 unique"))
        assertFalse(text, text.contains("candidate"))
    }

    @Test
    fun theTestSummaryFramesCandidatesAsReviewNotRemoval() {
        val text = CocoTestImpactPanel.summarize(candidates = 2, total = 9)
        assertTrue(text, text.contains("9 tests"))
        assertTrue(text, text.contains("2 contribute no unique coverage"))
        assertTrue("frames it as review: $text", text.contains("not deletion"))
    }

    @Test
    fun aSuiteWhereEveryTestPullsItsWeightSaysThatPlainly() {
        assertTrue(
            CocoTestImpactPanel.summarize(candidates = 0, total = 9)
                .contains("every one contributes something unique")
        )
    }

    private fun assertFalse(message: String, condition: Boolean) =
        assertEquals(message, false, condition)
}
