package dev.cajeta.idea.coverage

import java.io.File

/** The tests known to have covered one line, and whether that list is complete. */
data class TestsForLine(
    val file: String,
    val line: Int,
    /** Authoritative count, even when [tests] is truncated. */
    val testCount: Int,
    val tests: List<String>,
    val omitted: Int,
) {
    val isTruncated: Boolean get() = omitted > 0

    /** What to show, stating the truncation rather than implying completeness. */
    fun describe(): String = when {
        testCount == 0 -> "no test covers this line"
        isTruncated -> "${tests.joinToString(", ")} and $omitted more ($testCount total)"
        else -> tests.joinToString(", ")
    }
}

/**
 * Queries over `coco-attribution v1` (spec §6.2).
 *
 * The format's per-line test list is **truncated** by coco with a `+N` marker,
 * so it is a sample and the counts are authoritative. Every answer here carries
 * that distinction rather than flattening it: presenting a sample as the
 * complete set would be a quiet lie about which tests cover a line.
 */
class CocoAttributionModel(private val data: CocoAttribution) {

    val tests: List<CocoTestSummary> get() = data.summaries

    /**
     * The raw per-line attribution, for the cross-tab joins.
     *
     * Exposed rather than reached through `data` so callers see the
     * [CocoAttributedLine.isTruncated] flag on every row: coco caps the test
     * list per line, and a join that ignores that cap silently under-reports.
     */
    fun attributedLines(): List<CocoAttributedLine> = data.lines

    /** 6.2.1 — the tests that exercised a line. */
    fun testsCovering(file: String, line: Int): TestsForLine? =
        data.lines.firstOrNull { it.file == file && it.line == line }
            ?.let { TestsForLine(it.file, it.line, it.testCount, it.tests, it.omittedTests) }

    /**
     * 6.2.2 — the lines only [testName] covers.
     *
     * Derived from lines whose test count is exactly one, which is the only case
     * where the truncated list is provably complete: a one-entry list is never
     * abbreviated. A line covered by several tests cannot be attributed uniquely
     * to any of them, so it correctly does not appear.
     */
    fun uniqueLinesOf(testName: String): List<CocoAttributedLine> =
        data.lines.filter { it.testCount == 1 && it.tests.singleOrNull() == testName }
            .sortedWith(compareBy({ it.file }, { it.line }))

    /**
     * 6.2.3 — tests contributing no uniquely-covered line.
     *
     * A redundancy CANDIDATE, not a verdict: a test can be worth keeping for
     * what it asserts even when another test happens to execute the same lines.
     * Coverage overlap is evidence, not proof.
     */
    fun redundancyCandidates(): List<CocoTestSummary> =
        data.redundantTests().sortedBy { it.name }

    companion object {
        const val FILE_NAME: String = "attribution.tsv"

        /**
         * Load the attribution beside a run's site table, or null when the run
         * did not collect it.
         *
         * Null is a real answer here and must reach the view: attribution is
         * only produced when the suite ran under the per-test hook, and an empty
         * table would read as "no test covers anything" (spec §6.2.4).
         */
        fun beside(profile: File): CocoAttributionModel? {
            val siteTable = CocoArtifacts.locateSiteTable(profile) ?: return null
            val f = File(siteTable.parentFile, FILE_NAME)
            if (!f.isFile) return null
            return try {
                CocoAttributionModel(parseCocoAttribution(f.readText()))
            } catch (e: CocoFormatException) {
                null
            } catch (e: java.io.IOException) {
                null
            }
        }

        /** 6.2.4 — what to say when there is nothing to show. */
        const val NOT_COLLECTED: String =
            "This run did not collect per-test attribution. Re-run with the " +
                "per-test hook enabled to see which tests cover which lines."
    }
}
