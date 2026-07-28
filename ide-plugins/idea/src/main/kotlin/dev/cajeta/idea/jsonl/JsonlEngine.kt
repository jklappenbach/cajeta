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
            parseLine(lineNumber, line)?.let { rows += it }
        }
        return JsonlModel(rows, deriveColumns(rows))
    }

    /** Parse one physical line into a row (spec §8 windowing reuse): a JSON object
     *  becomes a [JsonlRow.Record], any other non-blank line a [JsonlRow.Raw]
     *  passthrough; a blank line carries nothing and returns null. Same rule as
     *  [parse], exposed so the windowed viewer renders identically to the console.
     *
     *  Strict-first (json-viewer spec §2.1.6): a well-formed JSONL stream pays only
     *  the strict parse. Only on a strict miss does classification try, in order,
     *  ANSI stripping, JSONC leniency, and a trailing JSON object behind a
     *  plain-text prefix; anything still unmatched is a raw passthrough. */
    fun parseLine(lineNumber: Int, line: String): JsonlRow? {
        if (line.isEmpty() || line.isBlank()) return null
        val obj = parseObjectOrNull(line)
        if (obj != null) return JsonlRow.Record(lineNumber, obj, line)
        return classifyLenient(lineNumber, line)
    }

    private val ANSI = Regex("\u001B\\[[0-9;?]*[ -/]*[@-~]")

    private fun classifyLenient(lineNumber: Int, line: String): JsonlRow {
        val stripped = if ('\u001B' in line) ANSI.replace(line, "") else line
        if ('{' !in stripped) return JsonlRow.Raw(lineNumber, line)
        if (stripped !== line) {
            parseObjectOrNull(stripped)?.let { return JsonlRow.Record(lineNumber, it, line) }
        }
        lenientObjectOrNull(stripped)?.let { return JsonlRow.Record(lineNumber, it, line) }
        // a trailing JSON object after a plain-text prefix (logger prefixes)
        var idx = stripped.indexOf('{')
        while (idx >= 0) {
            if (idx > 0) {
                val tail = stripped.substring(idx)
                val fields = parseObjectOrNull(tail) ?: lenientObjectOrNull(tail)
                if (fields != null) {
                    return JsonlRow.Record(lineNumber, fields, line, prefix = stripped.substring(0, idx))
                }
            }
            idx = stripped.indexOf('{', idx + 1)
        }
        return JsonlRow.Raw(lineNumber, line)
    }

    /** Parse a single line as a JSON object; null for non-JSON or non-object. */
    private fun parseObjectOrNull(line: String): Map<String, Json>? = try {
        (Json.parse(line) as? Json.Obj)?.entries
    } catch (_: Exception) {
        null
    }

    /** JSONC-lenient object parse (comments, trailing commas); null on any miss. */
    private fun lenientObjectOrNull(text: String): Map<String, Json>? {
        val ok = JsonDocModel.parse(text, lenient = true) as? JsonDocResult.Ok ?: return null
        return (ok.root.toJson() as? Json.Obj)?.entries
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
