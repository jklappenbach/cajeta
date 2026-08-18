package dev.cajeta.idea.coverage

import java.io.File

/**
 * One method's risk score, exactly as coco computed it.
 *
 * Units are coco's: the score in tenths and coverage in per-mille, because the
 * computation is integer-only on that side and reports are diffed in CI, where
 * float formatting drift shows up as phantom changes. Formatting for display is
 * this side's business; the number is not.
 */
data class CrapEntry(
    val method: String,
    val complexity: Int,
    val coveragePerMille: Int,
    val scoreTenths: Int,
) {
    /** `12.3`, from the integer coco published. */
    val score: String get() = "${scoreTenths / 10}.${scoreTenths % 10}"

    val coveragePercent: Int get() = coveragePerMille / 10

    /** The conventional CRAP attention threshold is 30. */
    val isHighRisk: Boolean get() = scoreTenths >= HIGH_RISK_TENTHS

    /**
     * 6.3.3 — the inputs, so the number is explicable rather than opaque.
     *
     * A ranking nobody can interrogate gets ignored; being able to see that a
     * score is high because complexity is 12 and coverage is 8% is what makes it
     * actionable.
     */
    fun explain(): String =
        "complexity $complexity, $coveragePercent% line coverage → CRAP $score"

    companion object {
        const val HIGH_RISK_TENTHS = 300
    }
}

class CocoFormatCrapException(message: String) : Exception(message)

/**
 * Reader for `coco-crap v1` (spec §6.3, plan 7.3.a).
 *
 * **The score is never recomputed here.** coco owns the metric's definition; a
 * second implementation in Kotlin would be a second definition, and two
 * definitions of a metric drift while both keep producing plausible-looking
 * numbers. So this parses and presents, and nothing else.
 *
 * The file's order IS the ranking — coco emits worst-first — so it is preserved
 * rather than re-sorted.
 */
object CocoCrap {

    const val HEADER: String = "coco-crap v1"
    const val FILE_NAME: String = "crap.tsv"

    /** 6.2.4's shape for this view: absent data is stated, not shown as empty. */
    const val NOT_AVAILABLE: String =
        "This run has no risk ranking. It is written by coco's report action, " +
            "so a run that only instrumented has not produced one."

    fun parse(text: String): List<CrapEntry> {
        val lines = text.split('\n')
        val header = lines.firstOrNull()
            ?: throw CocoFormatCrapException("coco-crap: empty document; expected \"$HEADER\"")
        if (header != HEADER) {
            throw CocoFormatCrapException(
                "coco-crap: unsupported header \"$header\"; this build reads only \"$HEADER\""
            )
        }
        val out = ArrayList<CrapEntry>()
        for (i in 1 until lines.size) {
            val line = lines[i]
            if (line.isEmpty()) continue
            val f = line.split('\t')
            if (f.size < 4) {
                throw CocoFormatCrapException(
                    "coco-crap: malformed row at line ${i + 1}; " +
                        "expected 4 tab-separated fields, found ${f.size}"
                )
            }
            out.add(
                CrapEntry(
                    method = f[0],
                    complexity = f[1].toIntOrNull()
                        ?: throw CocoFormatCrapException("coco-crap: non-numeric complexity at line ${i + 1}"),
                    coveragePerMille = f[2].toIntOrNull()
                        ?: throw CocoFormatCrapException("coco-crap: non-numeric coverage at line ${i + 1}"),
                    scoreTenths = f[3].toIntOrNull()
                        ?: throw CocoFormatCrapException("coco-crap: non-numeric score at line ${i + 1}"),
                )
            )
        }
        return out
    }

    /** The ranking beside a run's site table, or null when the run has none. */
    fun beside(profile: File): List<CrapEntry>? {
        val siteTable = CocoArtifacts.locateSiteTable(profile) ?: return null
        val f = File(siteTable.parentFile, FILE_NAME)
        if (!f.isFile) return null
        return try {
            parse(f.readText())
        } catch (e: CocoFormatCrapException) {
            null
        } catch (e: java.io.IOException) {
            null
        }
    }
}
