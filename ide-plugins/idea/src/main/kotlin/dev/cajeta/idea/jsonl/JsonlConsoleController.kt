package dev.cajeta.idea.jsonl

/**
 * The plain-JVM core of the structured JSONL console (spec §7). Streams process
 * output incrementally — chunks that need not break on line boundaries — into
 * the shared §8 [JsonlEngine] render model, and owns the view state: structured
 * vs raw mode and the active level/field filter. No `com.intellij.*`, so the
 * streaming + filtering logic is unit-tested independent of the console pane
 * ([JsonlConsolePanel] is a thin delegate over this).
 *
 * Incremental by construction: only completed lines (terminated by `\n`) are
 * parsed; a trailing partial line is held until the rest of it arrives, so a
 * record never materializes half-formed (§6.2.1, §7.2.1).
 */
class JsonlConsoleController(structuredByDefault: Boolean = true) {

    private val completed = StringBuilder()   // all newline-terminated text seen
    private val pending = StringBuilder()      // the not-yet-terminated trailing line

    // Rows parsed incrementally as lines complete (json-viewer §3.1.5): a 10k-line
    // burst refreshed per batch costs one parse per line, not a whole-buffer
    // reparse per refresh. Blank lines advance the counter but yield no row.
    private val rows = ArrayList<JsonlRow>()
    private var lineNumber = 0

    var isStructured: Boolean = structuredByDefault
        private set

    private var filter: ((JsonlRow) -> Boolean)? = null

    /** Feed a chunk of process output. Splits on newlines; the last fragment is
     *  buffered as pending unless the chunk ended on a line boundary. */
    fun append(chunk: String) {
        if (chunk.isEmpty()) return
        pending.append(chunk)
        var nl = pending.indexOf("\n")
        while (nl >= 0) {
            val line = pending.substring(0, nl)
            completed.append(pending, 0, nl + 1)   // include the newline
            pending.delete(0, nl + 1)
            lineNumber++
            JsonlEngine.parseLine(lineNumber, line)?.let { rows += it }
            nl = pending.indexOf("\n")
        }
    }

    fun setStructured(on: Boolean) { isStructured = on }

    fun setLevelFilter(minLevel: String) { filter = JsonlEngine.atOrAboveLevel(minLevel) }

    fun setFieldFilter(key: String, value: String) { filter = JsonlEngine.fieldEquals(key, value) }

    fun clearFilter() { filter = null }

    /** The full render model over everything completed so far (structured view). */
    fun model(): JsonlModel = JsonlModel(rows.toList(), JsonlEngine.columnsOf(rows))

    /** Rows to display in structured mode, after the active filter. */
    fun visibleRows(): List<JsonlRow> =
        filter?.let { rows.filter(it) } ?: rows.toList()

    /** The verbatim stream for raw mode (completed lines only; a partial trailing
     *  line is shown once it completes). */
    fun rawText(): String = completed.toString()

    /** Reset to empty (console Clear): both cards start over. */
    fun clear() {
        completed.setLength(0)
        pending.setLength(0)
        rows.clear()
        lineNumber = 0
    }
}
