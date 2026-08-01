package dev.cajeta.idea.jsonl

/**
 * Which fields a structured JSONL table shows, and how wide each needs to be
 * (json-viewer spec §3.1.7, §3.1.8). Pure — the Swing surfaces render what this
 * decides and add nothing of their own.
 *
 * A JSONL stream reveals its shape as it runs, so this is fed row by row rather
 * than derived from a finished model:
 * - **Discovery** ([available]) accrues every field ever seen, in the engine's
 *   deterministic order, and never shrinks — a field first seen at line 10,000
 *   joins the chooser and stays there even if no later record carries it.
 * - **Selection** ([visible]) starts as a SUBSET — the first [defaultCount] of
 *   that order — so a wide record shape doesn't open as a wall of columns. Until
 *   the reader picks, the default is recomputed on every discovery, so an
 *   opening metadata line can't fix a poor column set. The first explicit
 *   [setFieldVisible] pins the choice; later discoveries are then offered but
 *   stay off. [resetToDefaults] hands control back.
 * - **Width** ([widestCell]) tracks the longest RENDERED cell per field, updated
 *   once per row rather than recomputed per refresh — a 10k-line burst costs one
 *   comparison per field per row. Grow-only, so a streaming table never reflows
 *   narrower under the reader, and RECORD cells only: one long raw passthrough
 *   line must not push every structured column off-screen (§3.1.8.1).
 */
class JsonlColumns(defaultCount: Int = DEFAULT_COUNT) {

    /** How many columns the default selection shows. Mutable for tests and for
     *  a future preference; changing it re-derives an unpinned selection. */
    var defaultCount: Int = defaultCount

    private val discovered = LinkedHashSet<String>()
    private val widest = HashMap<String, String>()
    private var widestLine = ""
    private var ordered: List<String>? = null

    /** The reader's explicit selection, or null while the default applies. */
    private var pinned: LinkedHashSet<String>? = null

    /** Register one row's fields and cell widths. Raw passthrough rows carry
     *  neither, so they are ignored (§3.1.8.1). */
    fun observe(row: JsonlRow) {
        val line = when (row) {
            is JsonlRow.Record -> row.raw
            is JsonlRow.Raw -> row.text
        }
        if (line.length > widestLine.length) widestLine = line
        val record = row as? JsonlRow.Record ?: return
        for (key in record.fields.keys) {
            if (discovered.add(key)) ordered = null
            val cell = JsonlEngine.cell(record, key)
            val current = widest[key]
            if (current == null || cell.length > current.length) widest[key] = cell
        }
    }

    fun observeAll(rows: Iterable<JsonlRow>) = rows.forEach(::observe)

    /** Every field discovered so far, in the engine's deterministic order. */
    fun available(): List<String> =
        ordered ?: JsonlEngine.orderColumns(discovered).also { ordered = it }

    /** The columns to render, in that same order — never the toggle order. */
    fun visible(): List<String> {
        val chosen = pinned ?: return available().take(defaultCount)
        return available().filter { it in chosen }
    }

    fun isVisible(field: String): Boolean = field in visible()

    /** True once the reader has made an explicit choice (§3.1.7.2). */
    fun isPinned(): Boolean = pinned != null

    /** Show or hide one field. The first call pins the current selection, so
     *  toggling one field never silently re-derives the others. */
    fun setFieldVisible(field: String, show: Boolean) {
        val chosen = pinned ?: LinkedHashSet(visible()).also { pinned = it }
        if (show) chosen.add(field) else chosen.remove(field)
    }

    /** Drop the explicit selection and track the default again. */
    fun resetToDefaults() { pinned = null }

    /** The longest rendered cell seen for [field] (""; never null), the basis
     *  for an uncapped column width. */
    fun widestCell(field: String): String = widest[field] ?: ""

    /** The longest whole line seen, records and raw passthrough alike — the
     *  width basis for the single line-text column an empty selection renders
     *  (§3.1.7.3), where that text is the content rather than a neighbour. */
    fun widestLine(): String = widestLine

    companion object {
        /** Columns shown before the reader chooses. Enough for the common log
         *  shape (time, level, message and a little context) without filling
         *  the viewport. */
        const val DEFAULT_COUNT = 5
    }
}
