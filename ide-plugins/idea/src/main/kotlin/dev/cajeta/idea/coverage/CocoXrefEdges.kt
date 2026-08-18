package dev.cajeta.idea.coverage

import com.intellij.openapi.project.Project
import dev.cajeta.idea.debugger.Json
import dev.cajeta.idea.xref.CajetaXrefFreshness
import dev.cajeta.idea.xref.XrefQuery

/**
 * [XrefEdges] over the existing [XrefQuery] / `CajetaXrefIndex` (plan 6.2.a).
 *
 * Reuses the index rather than reparsing coco's `xref.json`: the index is
 * already incremental, already invalidated on edit, and already the plugin's
 * answer to "who calls this". A second parse of the same data would be a second
 * thing to keep in sync and a second thing to go stale.
 *
 * Keys are normalized to coco's arity-collapsed shape on the way in and out, so
 * both sides of the join speak one spelling ([CocoKeys]).
 */
class CocoXrefEdges(private val project: Project) : XrefEdges {

    private val callers = HashMap<String, List<String>>()
    private val bases = HashMap<String, List<String>>()
    private val known = HashMap<String, Boolean>()

    override fun callersOf(key: String): List<String> = callers.getOrPut(key) {
        expand(key) { overloadKey ->
            XrefQuery.callersOf(project, overloadKey).mapNotNull { field(it, "caller") }
        }
    }

    override fun basesOf(key: String): List<String> = bases.getOrPut(key) {
        expand(key) { overloadKey ->
            XrefQuery.overriddenBy(project, overloadKey).mapNotNull { field(it, "overrides") }
        }
    }

    override fun isKnown(key: String): Boolean = known.getOrPut(key) {
        overloadKeysFor(key).isNotEmpty()
    }

    /**
     * One arity key can stand for several real overloads, so every matching
     * overload is queried and the results unioned.
     *
     * Unioning is the conservative direction: it can only ADD callers, and an
     * extra caller can only move a verdict away from "dead".
     */
    private fun expand(key: String, query: (String) -> List<String>): List<String> =
        overloadKeysFor(key)
            .flatMap(query)
            .mapNotNull(CocoKeys::ofOverloadKey)
            .distinct()

    /**
     * The compiler-shaped overload keys that collapse to [key].
     *
     * Found by simple name and filtered by arity, because the index is keyed by
     * the compiler's spelling and coco's is lossy — there is no way back except
     * to look up the candidates and narrow.
     */
    private fun overloadKeysFor(key: String): List<String> {
        val sep = key.indexOf("::")
        if (sep <= 0) return emptyList()
        val owner = key.substring(0, sep)
        val name = key.substring(sep + 2).substringBefore('/')
        return XrefQuery.declarationsOf(project, "$owner.$name")
            .mapNotNull { field(it, "overloadKey") }
            .filter { CocoKeys.ofOverloadKey(it) == key }
            .distinct()
    }

    private fun field(o: Json.Obj, name: String): String? = (o.opt(name) as? Json.Str)?.value

    companion object {
        /**
         * Why reachability is unavailable, or null when it can be trusted.
         *
         * Reachability read off a stale index would classify recently-deleted
         * callers as still present and recently-added ones as absent — the
         * latter turning live code into a deletion candidate. Refusing wholesale
         * is spec §6.1.5, and it reuses the same freshness gate the refactoring
         * path already respects.
         */
        fun unavailableReason(project: Project): String? {
            val freshness = CajetaXrefFreshness.getInstance(project)
            return when (freshness.state) {
                CajetaXrefFreshness.State.FRESH -> null
                CajetaXrefFreshness.State.REFRESHING ->
                    "the Cajeta index is still refreshing — reachability is not yet decidable"
                CajetaXrefFreshness.State.STALE ->
                    "the Cajeta index is not up to date" +
                        (freshness.reason?.let { " ($it)" } ?: "") +
                        " — nothing is classified as unreachable on stale data"
                CajetaXrefFreshness.State.UNAVAILABLE ->
                    "reachability is unavailable: ${freshness.reason ?: "the index is not usable"}"
            }
        }
    }
}
