package dev.cajeta.idea.coverage

/**
 * The queries that make the four tabs answer each other (spec §6.2–§6.4).
 *
 * Each tab alone is a list of findings. The value coco claims over a coverage
 * viewer is the INTERSECTION: attribution raises a question that mutation
 * answers, and a mutation survivor points at the tests that should have caught
 * it. Until these existed, the tabs were four lists that happened to share a
 * window and the reader had to join them by eye.
 *
 * Pure by construction — every function takes the models and returns data, so
 * the joins are unit-testable without an IDE. The panels do the Swing.
 */
object CocoCrossLinks {

    /**
     * The tests that executed the line a mutant sits on.
     *
     * A SURVIVING mutant on a covered line is the sharpest finding coco has:
     * something ran that code and asserted nothing about it. These are the
     * tests that ran it, and therefore the ones an assertion belongs in.
     *
     * [TestsForLine.isTruncated] must be respected by the caller: coco caps the
     * per-line list and reports the true count separately, so a truncated
     * result is a SAMPLE. Presenting it as the full set would be a quiet lie
     * about which tests cover a line.
     */
    fun testsCovering(model: CocoAttributionModel?, mutant: MutantResult): TestsForLine? =
        model?.testsCovering(mutant.sourceFile, mutant.srcLine)

    /**
     * Mutants sitting on lines this test is recorded as covering.
     *
     * This is the answer to "zero unique coverage — should I delete it?", and
     * the answer is frequently NO. The tour proves it in its own fixture:
     * `PricingTests.discountAppliesAtTheThreshold` contributes no uniquely
     * covered line and is the only test that kills `Pricing`'s mutant. Overlap
     * is evidence of redundancy; killing a mutant is evidence of worth, and it
     * beats the overlap.
     *
     * ONLY AS COMPLETE AS THE ATTRIBUTION. coco truncates the per-line test
     * list, so a test omitted from a truncated line is invisible here — the
     * result can under-report and must never be presented as proof that a test
     * kills nothing. [linesAreComplete] says whether any line consulted was
     * truncated.
     */
    fun mutantsOnLinesOf(
        model: CocoAttributionModel?,
        mutants: List<MutantResult>?,
        testName: String,
    ): List<MutantResult> {
        if (model == null || mutants.isNullOrEmpty()) return emptyList()
        val covered = linesOf(model, testName)
        if (covered.isEmpty()) return emptyList()
        return mutants
            .filter { (it.sourceFile to it.srcLine) in covered }
            .sortedWith(compareBy({ it.sourceFile }, { it.srcLine }, { it.mutation }))
    }

    /** Survivors only — the actionable subset of [mutantsOnLinesOf]. */
    fun survivorsOnLinesOf(
        model: CocoAttributionModel?,
        mutants: List<MutantResult>?,
        testName: String,
    ): List<MutantResult> =
        mutantsOnLinesOf(model, mutants, testName)
            .filter { it.verdict == MutationVerdict.SURVIVED }

    /**
     * Whether every line consulted for [testName] carried a complete test list.
     *
     * False means the answer above is a lower bound. Callers say so rather than
     * rounding it up to a fact.
     */
    fun linesAreComplete(model: CocoAttributionModel?, testName: String): Boolean =
        model?.attributedLines()?.none { it.isTruncated && testName in it.tests } ?: true

    private fun linesOf(model: CocoAttributionModel, testName: String): Set<Pair<String, Int>> =
        model.attributedLines()
            .filter { testName in it.tests }
            .map { it.file to it.line }
            .toSet()

    /**
     * The first line of [method] the run never executed.
     *
     * The destination that matters for a high-CRAP method. Jumping to the
     * DECLARATION is the wrong place to land: the reader already knows where
     * the method is — what they need is the code no test reaches, which is
     * where the test they are about to write has to get to.
     *
     * Null when every line ran (a high score can come from complexity alone)
     * or when the method is not in the site table.
     */
    fun firstUncoveredLine(coverage: CocoCoverage?, method: String): CocoSite? {
        if (coverage == null) return null
        return coverage.sites
            .filter { it.isSourceLine && "${it.owner}.${it.method}" == method }
            .filter { coverage.lineHits(it.file, it.line) == 0L }
            .minByOrNull { it.line }
    }
}
