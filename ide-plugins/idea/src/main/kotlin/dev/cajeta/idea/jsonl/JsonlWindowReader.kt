package dev.cajeta.idea.jsonl

/**
 * Reads a JSONL source in bounded windows for the standalone viewer (spec
 * §8.2.2) — large files must open responsively, so we never materialize the
 * whole file: only the requested physical-line window (plus a one-line peek for
 * `hasMore`) is pulled from the lazy line [Sequence]. Rows render through the
 * shared §8 [JsonlEngine], so the windowed view matches the console (§8.2.3,
 * §15.5). Pure; no `com.intellij.*`.
 */
object JsonlWindowReader {

    /**
     * A window of the file. [startLine] is the 1-based physical line the window
     * begins at; [rows] are the parsed non-blank rows within it (blank lines
     * carry nothing); [columns] is the engine's deterministic order over those
     * rows; [hasMore] is true when at least one more line follows the window.
     */
    data class Window(
        val startLine: Int,
        val rows: List<JsonlRow>,
        val columns: List<String>,
        val hasMore: Boolean,
    )

    /**
     * Read [count] physical lines starting at 1-based [startLine] from [lines].
     * Reads lazily: skips to the window, takes its lines, peeks one beyond for
     * [Window.hasMore], and stops — it does not drain the sequence.
     */
    fun read(lines: Sequence<String>, startLine: Int, count: Int): Window {
        val rows = ArrayList<JsonlRow>(count.coerceAtMost(1024))
        var hasMore = false
        var physicalLine = 0
        val endExclusive = startLine + count
        val it = lines.iterator()
        while (it.hasNext()) {
            physicalLine++
            val line = it.next()
            if (physicalLine < startLine) continue
            if (physicalLine < endExclusive) {
                JsonlEngine.parseLine(physicalLine, line)?.let { rows += it }
            } else {
                hasMore = true   // one line past the window exists
                break
            }
        }
        return Window(startLine, rows, JsonlEngine.columnsOf(rows), hasMore)
    }
}
