package dev.cajeta.idea.jsonl

import dev.cajeta.idea.debugger.Json

/**
 * One rendered row of a JSONL stream. A [Record] is a parsed JSON object (one
 * log record); a [Raw] is any line that isn't a JSON object — interleaved plain
 * text — which is passed through verbatim, never dropped (spec §7.2.4).
 */
sealed class JsonlRow {
    abstract val lineNumber: Int

    data class Record(
        override val lineNumber: Int,
        val fields: Map<String, Json>,
        val raw: String,
        /** Plain-text logger prefix preceding the JSON tail on a mixed line
         *  (spec §2.1.6), ANSI escapes removed; "" for a pure JSON line. */
        val prefix: String = "",
    ) : JsonlRow() {
        /** The record's level (level/severity field), lowercased, or null. */
        val level: String?
            get() = (fields["level"] ?: fields["severity"])
                ?.let { it as? Json.Str }?.value?.lowercase()
    }

    data class Raw(override val lineNumber: Int, val text: String) : JsonlRow()
}

/**
 * The rendered model shared by both JSONL surfaces (console + standalone editor):
 * the rows plus the derived, deterministic column order.
 */
data class JsonlModel(val rows: List<JsonlRow>, val columns: List<String>)

/** Level-based row coloring class (json-viewer spec §3.1.3). */
enum class RowTint { NORMAL, WARN, ERROR }
