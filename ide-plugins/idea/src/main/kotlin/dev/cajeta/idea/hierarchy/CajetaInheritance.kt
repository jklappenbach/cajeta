package dev.cajeta.idea.hierarchy

import dev.cajeta.idea.debugger.Json

/**
 * The pure half of type hierarchy (ide-features spec, Unit 3): reading the
 * compiler's inheritance edges and walking them without cycling.
 *
 * The compiler exports every `extends` and `implements` edge with both
 * endpoints — the relation its own comments call the one hierarchy rests on —
 * so nothing here infers a relationship. It only decides shape and ordering,
 * and refuses to loop.
 */
object CajetaInheritance {

    /** One inheritance edge as exported. */
    data class Edge(
        val child: String,
        val parent: String,
        val isInterface: Boolean,
        val file: String?,
        val line: Int,
    )

    fun parse(record: Json.Obj): Edge? {
        val child = (record.entries["child"] as? Json.Str)?.value?.ifBlank { null } ?: return null
        val parent = (record.entries["parent"] as? Json.Str)?.value?.ifBlank { null } ?: return null
        val kind = (record.entries["kind"] as? Json.Str)?.value
        return Edge(
            child = child,
            parent = parent,
            // `implements` names an interface; `extends` a class. The export
            // distinguishes them, so the browser need not guess from naming.
            isInterface = kind == "implements",
            file = (record.entries["file"] as? Json.Str)?.value?.ifBlank { null },
            line = (record.entries["line"] as? Json.Num)?.value?.toInt() ?: 0,
        )
    }

    fun parseAll(records: List<Json.Obj>): List<Edge> = records.mapNotNull(::parse)

    /**
     * Direct parents of [fqn], classes before interfaces and each group
     * alphabetical — so a type implementing several interfaces lists all of
     * them in a stable order rather than index order.
     */
    fun parentsOf(edges: List<Edge>): List<Edge> =
        edges.distinctBy { it.parent }
            .sortedWith(compareBy({ it.isInterface }, { it.parent }))

    /** Direct children, alphabetical and de-duplicated. */
    fun childrenOf(edges: List<Edge>): List<Edge> =
        edges.distinctBy { it.child }.sortedBy { it.child }

    /**
     * Walk one direction to a full closure, breadth-first, visiting each type
     * once. A cycle in the recorded relation — which a broken or half-written
     * index can contain — must terminate the walk, not the IDE.
     */
    fun closure(
        start: String,
        limit: Int = MAX_NODES,
        step: (String) -> List<String>,
    ): List<String> {
        val seen = LinkedHashSet<String>()
        val queue = ArrayDeque<String>()
        queue += start
        seen += start
        val out = ArrayList<String>()
        while (queue.isNotEmpty() && out.size < limit) {
            val next = queue.removeFirst()
            for (adjacent in step(next)) {
                if (adjacent.isBlank() || !seen.add(adjacent)) continue
                out += adjacent
                queue += adjacent
            }
        }
        return out
    }

    /** A hierarchy wider or deeper than this stops expanding: an index that
     *  says a million things is broken, and a browser must not hang on it. */
    const val MAX_NODES = 2000

    /** The name a hierarchy node shows: the simple name, with the package
     *  trailing, the way Java's browser reads. */
    fun displayName(fqn: String): String {
        val simple = fqn.substringAfterLast('.')
        val pkg = fqn.substringBeforeLast('.', "")
        return if (pkg.isBlank() || pkg == fqn) fqn else "$simple  ($pkg)"
    }
}
