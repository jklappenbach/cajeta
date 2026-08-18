package dev.cajeta.idea.coverage

/** How a method relates to the program's live surface. */
enum class Reachability {
    /** Some root can reach it. */
    REACHABLE,

    /** No root can reach it — a deletion candidate. */
    UNREACHABLE,

    /** Not determinable. Never a deletion candidate. */
    UNKNOWN,
}

/**
 * The graph, as far as reachability needs it. An interface so the policy below
 * is testable without an IntelliJ index behind it.
 */
interface XrefEdges {
    /** Methods that call [key]. */
    fun callersOf(key: String): List<String>

    /** Methods [key] overrides — the base of a base→impl edge. */
    fun basesOf(key: String): List<String>

    /** Whether the index has any record of [key] at all. */
    fun isKnown(key: String): Boolean
}

/**
 * coco's static-reachability policy, mirrored so the IDE and coco's own HTML
 * report classify identically (plan 6.1.e).
 *
 * Three rules, all coco's:
 *
 *  1. **Edges** are calls, plus overrides as base→impl — the conservative
 *     virtual-dispatch approximation, where a reachable base reaches every
 *     override.
 *  2. **Roots** are the entry point plus **every method the profile shows
 *     executed**. Reflection and DI are invisible to a static graph, and
 *     executed code is ground truth.
 *  3. **The error direction is toward "untested"**: the analysis may miss dead
 *     code but must never call a live method dead.
 *
 * Rule 3 governs everything ambiguous here, because the output of this analysis
 * is a suggestion to delete code. Absence of evidence — an unknown key, no roots
 * at all — yields [Reachability.UNKNOWN], never [Reachability.UNREACHABLE].
 *
 * The search runs BACKWARD from the method in question rather than forward from
 * the roots: the uncovered set is small and the whole program is not, so this
 * asks the index only about methods it actually needs.
 */
class CocoReachability(
    private val edges: XrefEdges,
    private val roots: Set<String>,
) {
    private val cache = HashMap<String, Reachability>()

    fun of(key: String?): Reachability {
        if (key == null) return Reachability.UNKNOWN
        // With no roots the analysis has no ground to stand on; reporting
        // everything unreachable would be a catastrophic false positive.
        if (roots.isEmpty()) return Reachability.UNKNOWN
        cache[key]?.let { return it }

        val result = when {
            key in roots -> Reachability.REACHABLE
            !edges.isKnown(key) -> Reachability.UNKNOWN
            search(key) -> Reachability.REACHABLE
            else -> Reachability.UNREACHABLE
        }
        cache[key] = result
        return result
    }

    /** Backward breadth-first to any root. Visited-set bounded, so cycles end. */
    private fun search(start: String): Boolean {
        val seen = HashSet<String>().apply { add(start) }
        val queue = ArrayDeque<String>().apply { add(start) }
        while (queue.isNotEmpty()) {
            val here = queue.removeFirst()
            for (pred in edges.callersOf(here) + edges.basesOf(here)) {
                if (pred in roots) return true
                if (seen.add(pred)) queue.add(pred)
            }
        }
        return false
    }
}
