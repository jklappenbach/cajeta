package dev.cajeta.idea.coverage

import java.io.File

/**
 * What the suite did to a mutant.
 *
 * The three-way split is coco's, and it is the whole value of the view: a
 * SURVIVED mutant sits on a line the suite *executed* without noticing the
 * behaviour change, which is a different and more interesting failure than a
 * line no test reached (spec §6.4.3).
 */
enum class MutationVerdict {
    /** The suite noticed. Nothing to do. */
    KILLED,

    /** Executed, changed, and nobody complained — execution without verification. */
    SURVIVED,

    /** The line never ran, so the mutant could not be killed. Not a survivor. */
    SKIPPED_UNCOVERED,
}

/** One mutant, as coco's driver recorded it. */
data class MutantResult(
    /** The IR module coco mutated, e.g. `probe/Cond.ll`. */
    val module: String,
    val srcLine: Int,
    /** The applied mutation, `from->to` — e.g. `slt->sle`. */
    val mutation: String,
    val verdict: MutationVerdict,
    /** The enclosing function's mangled symbol. */
    val method: String,
) {
    /**
     * The source file this module came from.
     *
     * coco names modules by their `.ll`, one per source file, so the mapping is
     * mechanical. Navigation needs the `.cajeta`; opening the IR would be
     * technically correct and useless.
     */
    val sourceFile: String get() = module.removeSuffix(".ll") + ".cajeta"

    fun describe(): String = "$mutation survived at line $srcLine"
}

class CocoFormatMutationException(message: String) : Exception(message)

/**
 * Reader for coco's `mutation.tsv`.
 *
 * **Two shapes, both accepted.** The engine's `cajeta.coverage.mutate` action
 * writes a version marker (`coco-mutation v1`) followed by the column header,
 * matching the versioning coco's other three formats already carry. The shell
 * driver's older output starts straight at the column header and carries no
 * version at all.
 *
 * The unversioned form is read as legacy rather than refused, because refusing
 * it would break every run produced before the action existed. But it is a
 * weaker contract by exactly the amount the marker is worth: an incompatible
 * change to the legacy shape is undetectable, which is the whole reason the
 * marker was added.
 */
object CocoMutation {

    const val FILE_NAME: String = "mutation.tsv"
    const val VERSION: String = "coco-mutation v1"
    const val HEADER: String = "module\tsrcLine\tmutation\tverdict\tmethod"

    const val NOT_AVAILABLE: String =
        "This run has no mutation results. Mutation testing is a separate pass " +
            "(the `cajeta.coverage.mutate` action, or `coco mutate`), not part " +
            "of an ordinary coverage run."

    fun parse(text: String): List<MutantResult> {
        val lines = text.split('\n')
        val first = lines.firstOrNull()
            ?: throw CocoFormatMutationException("mutation.tsv: empty document")

        // A version line, when present, precedes the column header. Anything
        // that looks like a coco-mutation marker but is not v1 is REFUSED
        // rather than parsed hopefully — the rule the other three formats
        // follow, and for the same reason: a plausible wrong number is not
        // recoverable, a refusal is.
        val versioned = first.trim().startsWith("coco-mutation ")
        if (versioned && first.trim() != VERSION) {
            throw CocoFormatMutationException(
                "mutation.tsv: unsupported version \"${first.trim()}\"; " +
                    "this build reads \"$VERSION\""
            )
        }
        val headerAt = if (versioned) 1 else 0
        val header = lines.getOrNull(headerAt)
            ?: throw CocoFormatMutationException(
                "mutation.tsv: $VERSION with no column header")
        if (header.trim() != HEADER) {
            throw CocoFormatMutationException(
                "mutation.tsv: unexpected header \"$header\"; this build reads \"$HEADER\""
            )
        }
        val out = ArrayList<MutantResult>()
        for (i in (headerAt + 1) until lines.size) {
            val line = lines[i]
            if (line.isEmpty()) continue
            val f = line.split('\t')
            if (f.size < 5) {
                throw CocoFormatMutationException(
                    "mutation.tsv: malformed row at line ${i + 1}; " +
                        "expected 5 tab-separated fields, found ${f.size}"
                )
            }
            out.add(
                MutantResult(
                    module = f[0],
                    srcLine = f[1].toIntOrNull()
                        ?: throw CocoFormatMutationException(
                            "mutation.tsv: non-numeric srcLine at line ${i + 1}"),
                    mutation = f[2],
                    verdict = verdictOf(f[3])
                        ?: throw CocoFormatMutationException(
                            "mutation.tsv: unknown verdict \"${f[3]}\" at line ${i + 1}"),
                    method = f[4],
                )
            )
        }
        return out
    }

    /**
     * Verdict spellings are the driver's own. An unrecognised one is refused
     * rather than folded into a neighbour: silently reading a new verdict as
     * `killed` would understate the survivors, which is the one direction that
     * makes the suite look better than it is.
     */
    private fun verdictOf(raw: String): MutationVerdict? = when (raw) {
        "killed" -> MutationVerdict.KILLED
        "SURVIVED" -> MutationVerdict.SURVIVED
        "skipped-uncovered" -> MutationVerdict.SKIPPED_UNCOVERED
        else -> null
    }

    fun beside(profile: File): List<MutantResult>? {
        val siteTable = CocoArtifacts.locateSiteTable(profile) ?: return null
        val f = File(siteTable.parentFile, FILE_NAME)
        if (!f.isFile) return null
        return try {
            parse(f.readText())
        } catch (e: CocoFormatMutationException) {
            null
        } catch (e: java.io.IOException) {
            null
        }
    }

    /** 6.4.1 — the survivors, which are the only actionable rows. */
    fun survivors(all: List<MutantResult>): List<MutantResult> =
        all.filter { it.verdict == MutationVerdict.SURVIVED }
            .sortedWith(compareBy({ it.module }, { it.srcLine }))

    /**
     * The summary. States skipped separately, because folding it into either
     * other bucket misrepresents the suite: skipped mutants are a coverage
     * problem, survivors are an assertion problem, and they have different fixes.
     */
    fun summarize(all: List<MutantResult>): String {
        if (all.isEmpty()) return " No mutants generated."
        val killed = all.count { it.verdict == MutationVerdict.KILLED }
        val survived = all.count { it.verdict == MutationVerdict.SURVIVED }
        val skipped = all.count { it.verdict == MutationVerdict.SKIPPED_UNCOVERED }
        val scored = killed + survived
        val score = if (scored == 0) "no scoreable mutants" else "$killed/$scored killed"
        val tail = if (skipped > 0) ", $skipped skipped as uncovered" else ""
        return " $score, $survived survived$tail"
    }
}
