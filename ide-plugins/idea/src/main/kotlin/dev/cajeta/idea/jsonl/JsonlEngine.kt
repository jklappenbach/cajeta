package dev.cajeta.idea.jsonl

import dev.cajeta.idea.debugger.Json

/**
 * The single JSONL render engine behind both surfaces — the structured console
 * (§7) and the standalone `.jsonl` editor (§8). Pure (reuses the bundled [Json]);
 * no `com.intellij.*`. Guarantees (spec §8.3): deterministic column order, every
 * non-empty line preserved (a non-JSON-object line becomes a [JsonlRow.Raw]
 * passthrough, never dropped or erroring), and correct level/field filtering.
 */
object JsonlEngine {

    /** Columns surfaced first when present, in this order; the rest follow in
     *  first-appearance order. Keeps the common log shape readable. */
    private val PREFERRED = listOf(
        "timestamp", "time", "ts", "level", "severity", "action", "message", "msg",
    )

    fun parse(text: String): JsonlModel {
        val rows = ArrayList<JsonlRow>()
        var lineNumber = 0
        for (line in text.split('\n')) {
            lineNumber++
            if (line.isEmpty() || line.isBlank()) continue   // blank lines carry nothing
            val obj = parseObjectOrNull(line)
            rows += if (obj != null) {
                JsonlRow.Record(lineNumber, obj, line)
            } else {
                JsonlRow.Raw(lineNumber, line)
            }
        }
        return JsonlModel(rows, deriveColumns(rows))
    }

    /** Parse one physical line into a row (spec §8 windowing reuse): a JSON object
     *  becomes a [JsonlRow.Record], any other non-blank line a [JsonlRow.Raw]
     *  passthrough; a blank line carries nothing and returns null. Same rule as
     *  [parse], exposed so the windowed viewer renders identically to the console. */
    fun parseLine(lineNumber: Int, line: String): JsonlRow? {
        if (line.isEmpty() || line.isBlank()) return null
        val obj = parseObjectOrNull(line)
        return if (obj != null) JsonlRow.Record(lineNumber, obj, line) else JsonlRow.Raw(lineNumber, line)
    }

    /** Parse a single line as a JSON object; null for non-JSON or non-object. */
    private fun parseObjectOrNull(line: String): Map<String, Json>? = try {
        (Json.parse(line) as? Json.Obj)?.entries
    } catch (_: Exception) {
        null
    }

    /** Deterministic column order for a set of rows (preferred keys first, then
     *  first-appearance). Public so the windowed viewer derives columns the same
     *  way as the console (spec §8.2.3, §15.5). */
    fun columnsOf(rows: List<JsonlRow>): List<String> = deriveColumns(rows)

    private fun deriveColumns(rows: List<JsonlRow>): List<String> {
        val seen = LinkedHashSet<String>()
        for (row in rows) {
            if (row is JsonlRow.Record) seen.addAll(row.fields.keys)
        }
        val preferred = PREFERRED.filter { it in seen }
        val rest = seen.filter { it !in preferred }
        return preferred + rest
    }

    // --- filters (spec §7.2.2). A Raw passthrough row always survives a filter
    //     (it has no fields to match) so the verbatim stream is never hidden. ---

    private val LEVEL_ORDER = listOf("trace", "debug", "info", "warn", "warning", "error", "fatal")
    private fun rank(level: String?): Int =
        when (level?.lowercase()) {
            "warning" -> LEVEL_ORDER.indexOf("warn")
            else -> LEVEL_ORDER.indexOf(level?.lowercase() ?: "")
        }

    /** Keep records at or above [minLevel]; raw rows pass through. */
    fun atOrAboveLevel(minLevel: String): (JsonlRow) -> Boolean {
        val floor = rank(minLevel)
        return { row ->
            when (row) {
                is JsonlRow.Raw -> true
                is JsonlRow.Record -> {
                    val r = rank(row.level)
                    r >= 0 && r >= floor
                }
            }
        }
    }

    /** Keep records whose [key] equals [value]; raw rows pass through. */
    fun fieldEquals(key: String, value: String): (JsonlRow) -> Boolean = { row ->
        when (row) {
            is JsonlRow.Raw -> true
            is JsonlRow.Record ->
                (row.fields[key] as? Json.Str)?.value == value
        }
    }

    /** Render a record's cell for [column] as display text ("" when absent). */
    fun cell(record: JsonlRow.Record, column: String): String =
        when (val v = record.fields[column]) {
            null -> ""
            is Json.Str -> v.value
            is Json.Null -> "null"
            else -> v.toCompactString()   // nested object/array shown compactly; expandable in UI
        }
}
