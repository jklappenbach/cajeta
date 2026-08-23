package dev.cajeta.idea.coverage

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The joins that make the four tabs one tool (Tier 2).
 *
 * Pure: the panels do Swing, these functions do the reasoning, and the
 * reasoning is what can be wrong in a way nobody notices. The specific hazard
 * pinned here is TRUNCATION — coco caps the per-line test list and reports the
 * true count separately, so a join that treats the list as complete silently
 * under-reports and presents a lower bound as a fact.
 */
class CocoCrossLinksTest {

    private fun line(file: String, ln: Int, tests: List<String>, omitted: Int = 0) =
        CocoAttributedLine(file, ln, tests.size + omitted, tests, omitted)

    private fun model(vararg lines: CocoAttributedLine, summaries: List<CocoTestSummary> = emptyList()) =
        CocoAttributionModel(CocoAttribution(summaries, lines.toList()))

    private fun mutant(file: String, ln: Int, verdict: MutationVerdict, mutation: String = "sge->sgt") =
        MutantResult(
            module = file.removeSuffix(".cajeta") + ".ll",
            srcLine = ln,
            mutation = mutation,
            verdict = verdict,
            method = "m",
        )

    // ── Mutants ▸ tests covering this line ─────────────────────────────────

    @Test
    fun `a survivor names the tests that ran its line`() {
        val m = model(line("Shipping.cajeta", 25, listOf("ShippingTests.large", "ShippingTests.small")))
        val covering = CocoCrossLinks.testsCovering(m, mutant("Shipping.cajeta", 25, MutationVerdict.SURVIVED))
        assertEquals(listOf("ShippingTests.large", "ShippingTests.small"), covering?.tests)
        assertFalse("nothing was omitted, so the list is complete", covering!!.isTruncated)
    }

    @Test
    fun `a mutant on an unattributed line names nobody`() {
        val m = model(line("Shipping.cajeta", 25, listOf("t")))
        assertNull(
            "a line with no attribution must yield null, not an empty answer " +
                "presented as 'no tests cover it'",
            CocoCrossLinks.testsCovering(m, mutant("Shipping.cajeta", 99, MutationVerdict.SURVIVED)),
        )
    }

    // ── Tests ▸ survivors on this test's lines ─────────────────────────────

    @Test
    fun `survivors are found on the lines a test covers`() {
        val m = model(
            line("Pricing.cajeta", 10, listOf("PricingTests.threshold")),
            line("Pricing.cajeta", 11, listOf("PricingTests.threshold")),
            line("Other.cajeta", 5, listOf("OtherTests.x")),
        )
        val mutants = listOf(
            mutant("Pricing.cajeta", 10, MutationVerdict.SURVIVED),
            mutant("Pricing.cajeta", 11, MutationVerdict.KILLED),
            mutant("Other.cajeta", 5, MutationVerdict.SURVIVED),
        )
        val survivors = CocoCrossLinks.survivorsOnLinesOf(m, mutants, "PricingTests.threshold")
        assertEquals("only survivors, only this test's lines", 1, survivors.size)
        assertEquals(10, survivors.first().srcLine)
    }

    @Test
    fun `killed mutants are not offered as survivors`() {
        val m = model(line("Pricing.cajeta", 10, listOf("t")))
        val mutants = listOf(mutant("Pricing.cajeta", 10, MutationVerdict.KILLED))
        assertTrue(CocoCrossLinks.survivorsOnLinesOf(m, mutants, "t").isEmpty())
        assertEquals(
            "the unfiltered join still sees it",
            1, CocoCrossLinks.mutantsOnLinesOf(m, mutants, "t").size,
        )
    }

    /**
     * The reason [CocoCrossLinks.linesAreComplete] exists. A truncated line can
     * omit the very test being asked about, so the join is a LOWER BOUND and
     * the UI must not claim otherwise.
     */
    @Test
    fun `truncated attribution is reported as incomplete`() {
        val truncated = model(line("Pricing.cajeta", 10, listOf("t"), omitted = 4))
        assertFalse(
            "a line whose test list was capped cannot support a complete answer",
            CocoCrossLinks.linesAreComplete(truncated, "t"),
        )
        val whole = model(line("Pricing.cajeta", 10, listOf("t")))
        assertTrue(CocoCrossLinks.linesAreComplete(whole, "t"))
    }

    @Test
    fun `missing models yield nothing rather than throwing`() {
        assertTrue(CocoCrossLinks.mutantsOnLinesOf(null, emptyList(), "t").isEmpty())
        assertTrue(CocoCrossLinks.survivorsOnLinesOf(model(), null, "t").isEmpty())
        assertNull(CocoCrossLinks.testsCovering(null, mutant("A.cajeta", 1, MutationVerdict.SURVIVED)))
        assertTrue(CocoCrossLinks.linesAreComplete(null, "t"))
    }

    // ── Risk ▸ first uncovered line ────────────────────────────────────────

    private fun site(id: Long, file: String, ln: Int, owner: String, method: String) =
        CocoSite(id, CocoSiteKind.LINE, ln, -1, file, owner, method, "", "")

    @Test
    fun `the first uncovered line is the lowest one that never ran`() {
        val sites = listOf(
            site(0, "T.cajeta", 10, "p.T", "f"),
            site(1, "T.cajeta", 12, "p.T", "f"),
            site(2, "T.cajeta", 14, "p.T", "f"),
        )
        // 10 ran; 12 and 14 did not.
        val coverage = CocoCoverage(sites, CocoProfile(3, null, mapOf(0L to 1L, 1L to 0L, 2L to 0L)))
        val first = CocoCrossLinks.firstUncoveredLine(coverage, "p.T.f")
        assertEquals("must be the LOWEST uncovered line, not the first in file order", 12, first?.line)
    }

    @Test
    fun `a fully covered method has no uncovered line to go to`() {
        val sites = listOf(site(0, "T.cajeta", 10, "p.T", "f"))
        val coverage = CocoCoverage(sites, CocoProfile(1, null, mapOf(0L to 3L)))
        assertNull(
            "a high CRAP score with full coverage is complexity, not coverage — " +
                "sending the reader hunting for an uncovered line wastes their time",
            CocoCrossLinks.firstUncoveredLine(coverage, "p.T.f"),
        )
    }

    @Test
    fun `an unknown method yields nothing`() {
        val coverage = CocoCoverage(listOf(site(0, "T.cajeta", 10, "p.T", "f")), CocoProfile(1, null, mapOf(0L to 0L)))
        assertNull(CocoCrossLinks.firstUncoveredLine(coverage, "p.T.nosuch"))
        assertNull(CocoCrossLinks.firstUncoveredLine(null, "p.T.f"))
    }

    // ── what the actions SAY ───────────────────────────────────────────────
    //
    // Separated from Swing precisely so it is assertable. The wording carries
    // the honesty guarantee: a truncated join is a lower bound, and a message
    // that states it as a fact is the failure this whole tool exists to avoid.

    @Test
    fun `a complete join states a count, a truncated one states a bound`() {
        val test = CocoTestSummary("PricingTests.threshold", covered = 5, unique = 0)

        val whole = model(line("P.cajeta", 10, listOf(test.name)))
        val exact = CocoCrossActions.describeSurvivors(whole, test, survivors = 3, revealed = 3)
        assertTrue(exact, exact.contains("3 surviving mutant"))
        assertFalse("a complete join must not hedge", exact.contains("at least"))
        assertFalse(exact.contains("lower bound"))

        val capped = model(line("P.cajeta", 10, listOf(test.name), omitted = 7))
        val bound = CocoCrossActions.describeSurvivors(capped, test, survivors = 3, revealed = 3)
        assertTrue(bound, bound.contains("at least 3"))
        assertTrue("the reader must be told it is a bound", bound.contains("lower bound"))
    }

    @Test
    fun `no survivors never reads as proof the test is worthless`() {
        val test = CocoTestSummary("PricingTests.threshold", covered = 5, unique = 0)
        val whole = CocoCrossActions.describeNoSurvivors(
            model(line("P.cajeta", 10, listOf(test.name))), test,
        )
        assertTrue(whole, whole.contains("never proof"))

        val capped = CocoCrossActions.describeNoSurvivors(
            model(line("P.cajeta", 10, listOf(test.name), omitted = 2)), test,
        )
        assertTrue(capped, capped.contains("not proof that it kills nothing"))
    }

    @Test
    fun `an unreachable tab is reported as such, not as an empty result`() {
        val test = CocoTestSummary("t", covered = 1, unique = 0)
        assertTrue(
            CocoCrossActions.describeSurvivors(model(), test, survivors = 2, revealed = -1)
                .contains("Could not open"),
        )
        val covering = TestsForLine("A.cajeta", 3, testCount = 2, tests = listOf("a", "b"), omitted = 0)
        assertTrue(
            CocoCrossActions.describeCovering(covering, revealed = -1).contains("Could not open"),
        )
    }

    @Test
    fun `a truncated per-line list is described as a sample`() {
        val full = TestsForLine("A.cajeta", 3, testCount = 2, tests = listOf("a", "b"), omitted = 0)
        assertFalse(CocoCrossActions.describeCovering(full, 2).contains("sample"))

        val partial = TestsForLine("A.cajeta", 3, testCount = 9, tests = listOf("a", "b"), omitted = 7)
        val text = CocoCrossActions.describeCovering(partial, 2)
        assertTrue(text, text.contains("9 tests"))
        assertTrue("must say the list is not the full set", text.contains("sample"))
    }
}
