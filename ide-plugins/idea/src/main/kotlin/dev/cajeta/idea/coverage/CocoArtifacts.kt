package dev.cajeta.idea.coverage

import java.io.File

/**
 * Finding and loading the pair of files a coco run leaves behind.
 *
 * A coverage suite names one file, but reading coverage needs two: probe ids in
 * the profile are **positional against the site table**, so the pair must come
 * from the same run. coco's layout fixes their relationship:
 *
 * ```
 * <base>/sites.tsv
 * <base>/run/coco.profile          the whole run
 * <base>/run/coco.merged.profile   attribution-merged, when tests were tracked
 * <base>/run/coco-test-<name>.profile
 * ```
 *
 * There is no safe fallback when the table is missing. Loading the profile
 * anyway would attribute hits to whatever table happened to turn up, so the
 * answer is "nothing found" — a refusal is recoverable, a plausible wrong
 * number is not.
 */
object CocoArtifacts {

    const val SITE_TABLE_NAME: String = "sites.tsv"

    /**
     * The whole-run profile under a coco output directory, or null when the run
     * has not produced one.
     *
     * Prefers `coco.merged.profile`, which coco's own report path prefers: it is
     * what a run that tracked per-test data writes, and taking the plain one
     * instead would silently discard the attribution the run paid for.
     *
     * Per-test dumps (`coco-test-<name>.profile`) sit in the same directory and
     * are deliberately NOT candidates — one of those is a single test's
     * coverage, and reporting it as the run's would understate everything.
     */
    fun discoverProfile(outDir: File): File? {
        val run = File(outDir, "run")
        return sequenceOf("coco.merged.profile", "coco.profile")
            .map { File(run, it) }
            .firstOrNull { it.isFile }
    }

    /** The site table belonging to [profile], or null when there is none. */
    fun locateSiteTable(profile: File): File? {
        val dir = profile.absoluteFile.parentFile ?: return null
        // Beside the profile first: that is where a hand-assembled pair sits,
        // and a nearer table is the more specific answer.
        return sequenceOf(File(dir, SITE_TABLE_NAME), File(dir.parentFile, SITE_TABLE_NAME))
            .firstOrNull { it.isFile }
    }

    /**
     * Read [profile] together with its site table.
     *
     * Returns null when the table cannot be found. Throws [CocoFormatException]
     * when either file is present but unreadable as `coco-sites v1` /
     * `coco-profile v1` — an unrecognised version is refused, never guessed at.
     */
    fun load(profile: File): CocoCoverage? {
        val siteTable = locateSiteTable(profile) ?: return null
        val sites = parseCocoSites(siteTable.readText())
        val hits = parseCocoProfile(profile.readText())
        return CocoCoverage(sites, hits)
    }
}
