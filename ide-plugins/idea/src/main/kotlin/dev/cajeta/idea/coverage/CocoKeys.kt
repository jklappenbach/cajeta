package dev.cajeta.idea.coverage

/**
 * The method key coverage and reachability join on.
 *
 * coco keys methods as **`owner::name/argc`**, collapsing overloads by arity.
 * That is a deliberate policy, not an accident — in coco's own words, keys
 * collapse by arity "so the analysis can miss dead code but can never call a
 * live method dead". The compiler's xref `overloadKey` is the other shape
 * (`demo.Box::get(T)`, params as written), so joining the two means converting,
 * and the conversion inherits the same obligation: when in doubt, produce
 * nothing rather than a key that matches nothing.
 *
 * A key that matched nothing would read as "no callers", which reads as dead,
 * which invites deleting working code. That is why [ofOverloadKey] and [ofSite]
 * return null on anything they cannot parse.
 */
object CocoKeys {

    /** From a coco site's `owner` + `method` (`both(b:int32,a:int32)`). */
    fun ofSite(owner: String, method: String): String? {
        if (owner.isBlank()) return null
        val open = method.indexOf('(')
        val close = method.lastIndexOf(')')
        if (open < 0 || close < open) return null
        val name = method.substring(0, open).trim()
        if (name.isEmpty()) return null
        return "$owner::$name/${arityOf(method.substring(open + 1, close))}"
    }

    /** From the compiler's `overloadKey` (`demo.Box::get(T)`). */
    fun ofOverloadKey(overloadKey: String): String? {
        val sep = overloadKey.indexOf("::")
        if (sep <= 0) return null
        val owner = overloadKey.substring(0, sep)
        return ofSite(owner, overloadKey.substring(sep + 2))
    }

    /**
     * Parameters separated by top-level commas.
     *
     * Depth-tracked, because `Map<K,V>` is one parameter: counting its comma
     * would key the method at the wrong arity, match nothing, and report a live
     * method as unreachable.
     */
    private fun arityOf(params: String): Int {
        val trimmed = params.trim()
        if (trimmed.isEmpty()) return 0
        var depth = 0
        var count = 1
        for (c in trimmed) {
            when (c) {
                '<', '(', '[' -> depth++
                '>', ')', ']' -> depth--
                ',' -> if (depth == 0) count++
            }
        }
        return count
    }
}
