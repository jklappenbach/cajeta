package dev.cajeta.idea.coverage

/**
 * The model the IDE binds to, read from cajeta-coco's published artifacts.
 *
 * Deliberately free of IntelliJ types: this is a pure parse + join, so it runs
 * off the EDT, is unit-testable without a platform fixture, and can be reused by
 * anything else that wants coco data (ide-coverage-plan 2.1.h, spec §7.3).
 *
 * The formats are specified in cajeta-coco `docs/formats.md`. Everything here was
 * written against that document and its conformance fixture rather than against
 * coco's source, which is the point of publishing them (plan 1.3.b).
 */

/** A probe kind. Unknown values are refused, never coerced — see [CocoFormatException]. */
enum class CocoSiteKind(val wire: String) {
    FUNCTION("function"),
    LINE("line"),
    BRANCH_TRUE("branch-true"),
    BRANCH_FALSE("branch-false");

    val isBranch: Boolean get() = this == BRANCH_TRUE || this == BRANCH_FALSE

    companion object {
        private val byWire = entries.associateBy { it.wire }
        fun of(wire: String): CocoSiteKind? = byWire[wire]
    }
}

/**
 * One probe site.
 *
 * NOTE on [decision]: coco declares it as the decision-grouping field, but the
 * current engine leaves it **-1 on every row**, branches included. Arms are
 * therefore paired by `(file, owner, method, block)`. A basic block ends in at
 * most one conditional branch, but block NAMES repeat across methods — every
 * method has an `entry` — so the enclosing method is part of the key. Do not
 * group on [decision] until coco populates it.
 */
data class CocoSite(
    val id: Long,
    val kind: CocoSiteKind,
    val line: Int,
    val decision: Long,
    val file: String,
    val owner: String,
    val method: String,
    val block: String,
    val target: String,
)

/**
 * Hit counts for one run, or for one test when [label] is set.
 *
 * `size` is the probe-table length coco declared. A probe id absent from [hits]
 * was never hit — that is normal, not a parse failure, so callers must read
 * through [countOf] rather than indexing.
 */
data class CocoProfile(
    val size: Long,
    val label: String?,
    val hits: Map<Long, Long>,
) {
    fun countOf(id: Long): Long = hits[id] ?: 0L
}

/**
 * How a branch arm fared. The distinction matters and coverage tools routinely
 * lose it: an arm that was never *reached* is a different finding from one that
 * was reached and never taken. LCOV encodes it as `-` versus `0`.
 */
enum class BranchOutcome { NOT_EVALUATED, NOT_TAKEN, TAKEN }

/** Identity of one decision. Block names repeat across methods, so the method is part of it. */
private data class BranchKey(val file: String, val owner: String, val method: String, val block: String)

/** One decision's two arms, resolved against a profile. */
data class CocoBranch(
    val file: String,
    val owner: String,
    /** Enclosing method — part of the key, since block names repeat across methods. */
    val method: String,
    /** The basic block the branch sits in. */
    val block: String,
    val line: Int,
    val trueOutcome: BranchOutcome,
    val falseOutcome: BranchOutcome,
) {
    val isFullyCovered: Boolean
        get() = trueOutcome == BranchOutcome.TAKEN && falseOutcome == BranchOutcome.TAKEN
    val isPartial: Boolean
        get() = !isFullyCovered &&
            (trueOutcome == BranchOutcome.TAKEN || falseOutcome == BranchOutcome.TAKEN)
}

/**
 * Sites joined to a profile.
 *
 * The join key is the probe id, which is dense within a run but **not durable
 * across runs** — so a profile may only ever be joined to the site table it was
 * produced with.
 */
class CocoCoverage(
    val sites: List<CocoSite>,
    val profile: CocoProfile,
) {
    private val byId: Map<Long, CocoSite> = sites.associateBy { it.id }

    val files: List<String> = sites.map { it.file }.distinct().sorted()

    /**
     * Hit count for a source line: the **maximum** over that line's probes,
     * never the sum.
     *
     * Several probes can sit on one line, so summing invents executions — a line
     * run once would report as run many times. coco's own LCOV emitter takes the
     * max for the same reason (spec §3.4).
     */
    fun lineHits(file: String, line: Int): Long =
        sites.asSequence()
            .filter { it.file == file && it.line == line }
            .maxOfOrNull { profile.countOf(it.id) } ?: 0L

    /** Lines carrying at least one probe, i.e. the lines coverage can speak to. */
    fun instrumentedLines(file: String): List<Int> =
        sites.asSequence().filter { it.file == file }.map { it.line }.distinct().sorted().toList()

    fun isLineCovered(file: String, line: Int): Boolean = lineHits(file, line) > 0

    /**
     * Branch outcomes per decision.
     *
     * An arm is TAKEN when its own probe was hit. When neither arm was hit the
     * decision was never reached, so both are NOT_EVALUATED. When one was hit and
     * the other was not, the decision *was* reached and the other arm is
     * NOT_TAKEN — a genuinely different finding.
     */
    fun branches(): List<CocoBranch> =
        sites.asSequence()
            .filter { it.kind.isBranch }
            .groupBy { BranchKey(it.file, it.owner, it.method, it.block) }
            .mapNotNull { (key, arms) ->
                val t = arms.firstOrNull { it.kind == CocoSiteKind.BRANCH_TRUE }
                val f = arms.firstOrNull { it.kind == CocoSiteKind.BRANCH_FALSE }
                if (t == null || f == null) return@mapNotNull null
                val tc = profile.countOf(t.id)
                val fc = profile.countOf(f.id)
                val reached = tc > 0 || fc > 0
                CocoBranch(
                    file = key.file,
                    owner = key.owner,
                    method = key.method,
                    block = key.block,
                    line = t.line,
                    trueOutcome = outcome(tc, reached),
                    falseOutcome = outcome(fc, reached),
                )
            }
            .sortedWith(compareBy({ it.file }, { it.line }, { it.method }, { it.block }))
            .toList()

    private fun outcome(count: Long, decisionReached: Boolean): BranchOutcome = when {
        count > 0 -> BranchOutcome.TAKEN
        decisionReached -> BranchOutcome.NOT_TAKEN
        else -> BranchOutcome.NOT_EVALUATED
    }

    fun siteById(id: Long): CocoSite? = byId[id]

    /** Probes hit at least once, over probes present in the site table. */
    fun coveredProbeCount(): Int = sites.count { profile.countOf(it.id) > 0 }
}

/** One test's contribution, from `coco-attribution v1`. */
data class CocoTestSummary(val name: String, val covered: Long, val unique: Long)

/**
 * Per-line attribution.
 *
 * [tests] is **truncated by coco** with a `+N` overflow marker, so it is a sample
 * and not the full set; [testCount] is authoritative. Presenting the sample as
 * complete would be a quiet lie about which tests cover a line.
 */
data class CocoAttributedLine(
    val file: String,
    val line: Int,
    val testCount: Int,
    val tests: List<String>,
    val omittedTests: Int,
) {
    val isTruncated: Boolean get() = omittedTests > 0
}

data class CocoAttribution(
    val summaries: List<CocoTestSummary>,
    val lines: List<CocoAttributedLine>,
) {
    /** Tests contributing no uniquely-covered line — redundancy candidates. */
    fun redundantTests(): List<CocoTestSummary> = summaries.filter { it.unique == 0L }
}
