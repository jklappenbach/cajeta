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
        /** The compiler-stream record type (compiler-jsonl §2/§3), or null for
         *  a stream that carries no discriminator — third-party JSONL, which
         *  the console renders too and which will never have one. */
        val kind: String?
            get() = (fields["kind"] as? Json.Str)?.value

        /**
         * The record's level, lowercased, or null.
         *
         * Dispatch on `kind` where there is one: a `diagnostic` carries
         * `severity`, a `log` carries `level`, and a `stream`/`cache` carries
         * neither and is metadata rather than a levelled row. Everything else
         * falls back to probing both field names — the shape third-party
         * emitters use (cajeta-logging: `{"ts":…,"level":"INFO",…}`). The
         * fallback is permanent by design, not a migration bridge: `kind`
         * makes the COMPILER's records dispatchable, it does not oblige
         * anyone else to carry one (spec 6.1.2).
         */
        val level: String?
            get() {
                val str = { key: String -> (fields[key] as? Json.Str)?.value?.lowercase() }
                return when (kind) {
                    "diagnostic" -> str("severity")
                    "log" -> str("level")
                    "stream", "cache", "progress", "result", "xref" -> null
                    else -> str("level") ?: str("severity")
                }
            }

        /** Terminal-record status (`ok` / `error`), or null when not a result. */
        val resultStatus: String?
            get() = if (kind == "result") (fields["status"] as? Json.Str)?.value?.lowercase()
                    else null
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
