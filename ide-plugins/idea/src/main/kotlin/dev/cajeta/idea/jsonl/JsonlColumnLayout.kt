package dev.cajeta.idea.jsonl

import dev.cajeta.idea.debugger.Json

/**
 * A saved table layout for one run/debug profile (json-viewer spec §3.1.9):
 * which columns are shown, in what ORDER, and any width the reader set by
 * dragging. Pure and self-encoding — the platform service that persists it
 * stores an opaque string and knows nothing about columns.
 *
 * Encoded as JSON rather than a delimited string because column names are
 * arbitrary JSON keys: quotes, pipes, colons and unicode all occur, and a
 * `name:width|name:width` scheme silently loses them.
 */
data class JsonlColumnLayout(
    /** Visible columns in display order. Empty is legal (§3.1.7.3). */
    val columns: List<String>,
    /** Reader-set widths, in pixels. Columns sized to content are absent. */
    val widths: Map<String, Int>,
) {
    fun encode(): String {
        val widthObj = Json.obj(*widths.map { (k, v) -> k to Json.of(v) }.toTypedArray())
        return Json.obj(
            "columns" to Json.arr(*columns.map { Json.of(it) }.toTypedArray()),
            "widths" to widthObj,
        ).toCompactString()
    }

    companion object {
        /**
         * Decode a stored payload, or null if there isn't a usable one.
         * Never throws: a layout is a convenience, and a corrupt one must
         * degrade to the defaults rather than break a console (§3.1.9.5).
         */
        fun decode(encoded: String?): JsonlColumnLayout? {
            if (encoded.isNullOrBlank()) return null
            val root = try {
                Json.parse(encoded) as? Json.Obj
            } catch (_: Exception) {
                null
            } ?: return null
            val columnsArr = root.entries["columns"] as? Json.Arr ?: return null
            val columns = columnsArr.items.mapNotNull { (it as? Json.Str)?.value }
            val widths = LinkedHashMap<String, Int>()
            (root.entries["widths"] as? Json.Obj)?.entries?.forEach { (name, value) ->
                val px = (value as? Json.Num)?.value?.toInt() ?: return@forEach
                if (px > 0) widths[name] = px
            }
            return JsonlColumnLayout(columns, widths)
        }
    }
}
