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

    /** Display order from a restored layout (§3.1.9), or null to use the
     *  engine's deterministic order. Columns absent from it follow, ordered
     *  deterministically, so a field discovered after the layout was saved
     *  still has a defined place. */
    private var order: List<String>? = null

    /** Widths the reader set by dragging (§3.1.9.3). Content-sized columns
     *  are deliberately absent: sizing one must not pin it. */
    private val userWidths = LinkedHashMap<String, Int>()

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

    /** The columns to render. Order is the restored layout's when there is
     *  one, otherwise the engine's deterministic order — never the order the
     *  reader happened to toggle things in. */
    fun visible(): List<String> {
        val chosen = pinned ?: return available().take(defaultCount)
        val byDiscovery = available().filter { it in chosen }
        val explicit = order ?: return byDiscovery
        // A remembered column this run never emitted still shows (§3.1.9.4):
        // the run may be the anomaly, and dropping it silently would make the
        // setting feel unreliable.
        val ordered = explicit.filter { it in chosen }
        return ordered + byDiscovery.filter { it !in ordered }
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

    /** Reorder the visible columns (a header drag). Pins the selection, as
     *  any explicit arrangement does. */
    fun setOrder(columns: List<String>) {
        if (pinned == null) pinned = LinkedHashSet(visible())
        order = columns.toList()
    }

    /** Record a width the reader set by dragging. */
    fun setUserWidth(field: String, px: Int) {
        if (px > 0) userWidths[field] = px else userWidths.remove(field)
    }

    /** That width, or null when this column has never been resized by hand
     *  and so still sizes to content (§3.1.8). */
    fun userWidth(field: String): Int? = userWidths[field]

    /** Drop the explicit selection, the order and the widths, and track the
     *  default again. */
    fun resetToDefaults() {
        pinned = null
        order = null
        userWidths.clear()
    }

    /** Capture what is on screen right now, for persistence (§3.1.9). Safe
     *  before the reader has touched anything: it captures the default, which
     *  is exactly what they are looking at. */
    fun currentLayout(): JsonlColumnLayout =
        JsonlColumnLayout(visible(), LinkedHashMap(userWidths))

    /** Restore a saved layout: the selection and its order arrive PINNED
     *  (§3.1.9.2), so a field switched off stays off however many later
     *  records carry it. */
    fun applyLayout(layout: JsonlColumnLayout) {
        pinned = LinkedHashSet(layout.columns)
        order = layout.columns.toList()
        userWidths.clear()
        userWidths.putAll(layout.widths.filterValues { it > 0 })
    }

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
