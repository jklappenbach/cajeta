package dev.cajeta.idea.debugger

/**
 * The discoverable legend for the memory-facet visualization (CP7-5, FR-7.2):
 * what each ownership / allocation / lifetime tag means. Platform-free so it
 * unit-tests directly and can be rendered anywhere (settings page, tool window,
 * docs). The tags here are exactly the ones [present] emits, so the legend and
 * the live decorations can never drift (single vocabulary).
 *
 * FR-7.4: every facet is named in text, so meaning is carried by the tag — never
 * by color alone. The legend states this so the guarantee is discoverable.
 */
data class LegendEntry(val term: String, val meaning: String)

object MemoryFacetLegend {

    val ownership: List<LegendEntry> = listOf(
        LegendEntry("owner", "sole binding responsible for dropping the value (shown emphasized)"),
        LegendEntry("borrow", "non-owning, aliasing reference"),
        LegendEntry("moved", "ownership transferred away via #"),
    )

    val allocation: List<LegendEntry> = listOf(
        LegendEntry("stack", "frame-local inline value"),
        LegendEntry("heap", "heap object or array reference"),
        LegendEntry("shared", "XPU shared placement (kernel-launch lifetime)"),
    )

    val lifetime: List<LegendEntry> = listOf(
        LegendEntry("live", "holds a valid value, in scope"),
        LegendEntry("about-to-drop", "live owner scheduled to drop at scope exit"),
        LegendEntry("moved-out", "consumed; struck through — reading it is an error"),
    )

    /** A readable multi-section legend, used by the settings page. */
    fun text(): String = buildString {
        appendSection(this, "Ownership", ownership)
        append('\n')
        appendSection(this, "Allocation", allocation)
        append('\n')
        appendSection(this, "Lifetime", lifetime)
        append('\n')
        append("Every facet is shown as a text tag, so meaning never depends on color alone.")
    }

    private fun appendSection(sb: StringBuilder, title: String, entries: List<LegendEntry>) {
        sb.append(title).append(':').append('\n')
        for (e in entries) {
            sb.append("  ").append(e.term).append(" — ").append(e.meaning).append('\n')
        }
    }
}
