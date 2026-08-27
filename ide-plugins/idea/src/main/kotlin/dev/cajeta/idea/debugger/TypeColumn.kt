package dev.cajeta.idea.debugger

/**
 * variable-inspection §4.1.5 — the Variables type column shows the simple
 * declared type (`String[]`, `Point`), never `cajeta.lang.String[]`.
 *
 * The server sends the canonical FQN on purpose: `ValueInspector::runtimeType`
 * re-narrows through the vtable on every decode, and the canonical name is what
 * identifies the type unambiguously across a classpath. Shortening it is a
 * presentation choice, so it lives on this side of the wire — the handle the
 * server mints still carries the full declared name.
 *
 * Shortening is applied to every qualified name inside the type, not just the
 * outermost one, so a generic reads `HashMap<String, Point>` rather than
 * `HashMap<cajeta.lang.String, tour.Point>` — the qualification is exactly as
 * uninformative in an argument position as it is in the head.
 */
object TypeColumn {

    /** Characters that separate one qualified name from the next. */
    private const val SEPARATORS = "<>,[] "

    /**
     * Reduce every dotted qualified name in [type] to its last segment,
     * preserving generics, array suffixes, and spacing verbatim. A segment that
     * would shorten to nothing (a trailing dot) is left alone — a readable
     * malformed name beats an empty column.
     */
    fun short(type: String): String {
        if (type.isBlank()) return ""
        val out = StringBuilder(type.length)
        val name = StringBuilder()
        fun flush() {
            if (name.isEmpty()) return
            val last = name.toString().substringAfterLast('.')
            out.append(if (last.isEmpty()) name else last)
            name.setLength(0)
        }
        for (ch in type) {
            if (ch in SEPARATORS) {
                flush()
                out.append(ch)
            } else {
                name.append(ch)
            }
        }
        flush()
        return out.toString()
    }
}
