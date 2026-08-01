package dev.cajeta.idea.jsonl

import dev.cajeta.idea.debugger.Json

/** A source location a structured row points at; line/column 1-based. */
data class RowLocation(val file: String, val line: Int, val column: Int)

/**
 * Diagnostic row navigation (json-viewer spec §3.1.6): the payoff of tools
 * encoding output as JSONL. Extraction and resolution are pure — the panel
 * layer does the actual OpenFileDescriptor jump. The recognized shape is the
 * compiler's NDJSON diagnostic (`file`, `line`, `column`, 1-based; `line`/
 * `column` optional), which lint output shares.
 */
object JsonlRowNavigation {

    /** The row's source coordinates, or null when it carries none. */
    fun locationOf(row: JsonlRow): RowLocation? {
        if (row !is JsonlRow.Record) return null
        val file = (row.fields["file"] as? Json.Str)?.value ?: return null
        if (file.isBlank()) return null
        val line = (row.fields["line"] as? Json.Num)?.value?.toInt() ?: 1
        val column = (row.fields["column"] as? Json.Num)?.value?.toInt() ?: 1
        return RowLocation(file, line.coerceAtLeast(1), column.coerceAtLeast(1))
    }

    /**
     * Resolve the location's file to a real path the way console file links do:
     * an absolute path stands if it exists; a relative one is tried against the
     * [roots] in order. Null = unresolvable = the row is not clickable (§3.1.2).
     */
    fun resolve(location: RowLocation, roots: List<String>, exists: (String) -> Boolean): String? {
        val f = location.file
        if (f.startsWith("/")) return f.takeIf(exists)
        for (root in roots) {
            val candidate = root.trimEnd('/') + "/" + f
            if (exists(candidate)) return candidate
        }
        return null
    }
}
