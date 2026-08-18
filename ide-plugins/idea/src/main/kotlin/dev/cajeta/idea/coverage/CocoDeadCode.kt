package dev.cajeta.idea.coverage

/**
 * What an uncovered method needs: a test, or deleting.
 *
 * The distinction is coco's headline capability and the reason this view exists
 * rather than pointing an lcov viewer at the export (spec §6.1). An lcov report
 * shows both as the same red — an equal-looking gap that is in one case work to
 * add and in the other work to remove.
 */
enum class Verdict {
    /** Statically unreachable: nothing can call it. Delete rather than test. */
    DELETION_CANDIDATE,

    /** Reachable but never executed. A test is the fix. */
    NEEDS_A_TEST,

    /** Reachability could not be determined. Neither claim is made. */
    UNDETERMINED,
}

/** One uncovered method, classified. */
data class UncoveredMethod(
    val key: String?,
    val owner: String,
    val method: String,
    val file: String,
    val line: Int,
    val verdict: Verdict,
    /** Why this verdict — shown so the classification is explicable, not opaque. */
    val reason: String,
) {
    /** Display name, `owner.method`. */
    val displayName: String get() = "$owner.$method"
}

/**
 * Classifies the uncovered methods of a run.
 *
 * Roots follow coco exactly: the declared entry point, **plus every method the
 * profile shows executed**. Seeding from coverage is what keeps reflection- and
 * DI-invoked code out of the deletion list — a static graph cannot see those
 * call sites, but the profile can see that the method ran.
 */
object CocoDeadCode {

    /**
     * @param entryKeys normalized keys of the program's entry point(s).
     * @param indexReason non-null when reachability is unavailable wholesale —
     *   a stale or absent index, say. Everything is then [Verdict.UNDETERMINED]
     *   with that reason, rather than every method being reported as dead.
     */
    fun classify(
        coverage: CocoCoverage,
        edges: XrefEdges,
        entryKeys: Set<String> = emptySet(),
        indexReason: String? = null,
    ): List<UncoveredMethod> {
        val byMethod = coverage.sites.groupBy { Triple(it.file, it.owner, it.method) }

        val executed = byMethod.entries
            .filter { (_, sites) -> sites.any { coverage.profile.countOf(it.id) > 0 } }
            .mapNotNull { (id, _) -> CocoKeys.ofSite(id.second, id.third) }
            .toSet()

        val reach = CocoReachability(edges, entryKeys + executed)

        return byMethod.entries
            .filter { (_, sites) -> sites.none { coverage.profile.countOf(it.id) > 0 } }
            .map { (id, sites) ->
                val (file, owner, method) = id
                val key = CocoKeys.ofSite(owner, method)
                val verdict = if (indexReason != null) Reachability.UNKNOWN else reach.of(key)
                UncoveredMethod(
                    key = key,
                    owner = owner,
                    method = method,
                    file = file,
                    // Function probes carry line 0, so the first real source
                    // line is what navigation can actually land on.
                    line = sites.filter { it.isSourceLine }.minOfOrNull { it.line } ?: 0,
                    verdict = verdictOf(verdict),
                    reason = reasonFor(verdict, key, indexReason),
                )
            }
            .sortedWith(compareBy({ it.file }, { it.line }, { it.method }))
    }

    private fun verdictOf(r: Reachability): Verdict = when (r) {
        Reachability.UNREACHABLE -> Verdict.DELETION_CANDIDATE
        Reachability.REACHABLE -> Verdict.NEEDS_A_TEST
        Reachability.UNKNOWN -> Verdict.UNDETERMINED
    }

    private fun reasonFor(r: Reachability, key: String?, indexReason: String?): String = when {
        indexReason != null -> indexReason
        r == Reachability.UNREACHABLE ->
            "no call path reaches it from the entry point or from any executed method"
        r == Reachability.REACHABLE ->
            "reachable from live code but never executed — a test is the fix"
        key == null -> "its signature could not be keyed, so reachability was not attempted"
        else -> "the index has no record of it, so reachability is unknown"
    }
}
