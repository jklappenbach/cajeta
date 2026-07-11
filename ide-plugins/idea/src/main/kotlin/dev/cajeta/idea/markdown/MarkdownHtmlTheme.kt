package dev.cajeta.idea.markdown

/**
 * Wraps engine-produced markdown HTML in a themed document for in-editor
 * rendering. Pure (no `com.intellij.*`, no Swing) so the CSS contract is
 * unit-tested: the same styled HTML feeds whichever surface renders it — the
 * Swing `JEditorPane` fold view today, a JCEF view later — so they stay
 * visually consistent.
 *
 * The CSS targets the lowest common denominator (Swing's `HTMLEditorKit`, a
 * partial CSS engine) while still styling the GFM constructs that matter for
 * code comments: tables, fenced code, inline code, blockquotes, headings, lists,
 * and rules. A JCEF surface renders the very same HTML with full fidelity.
 */
object MarkdownHtmlTheme {

    /** Colors + font pulled from the active editor scheme by the caller, so the
     *  rendered block matches the surrounding editor rather than looking like a
     *  foreign web widget. All colors are `#rrggbb`. */
    data class Palette(
        val foreground: String,
        val muted: String,
        val accent: String,
        val codeBackground: String,
        val border: String,
        val fontName: String,
        /** Editor font size in points. **Float**, not Int: IntelliJ's zoom
         *  (Ctrl+wheel, presentation mode, fractional IDE scaling) sets a
         *  fractional size, and rounding it to an Int makes sub-point zoom steps
         *  invisible to the render. Swing's `HTMLEditorKit` CSS parser accepts a
         *  fractional `pt` length, so this is emitted as-is. */
        val fontSizePt: Float,
        /** Body background. Null = transparent (Swing, painted over the fold tint);
         *  a color = opaque (JCEF, which has no transparent editor behind it). */
        val background: String? = null,
    )

    fun wrap(bodyHtml: String, p: Palette): String {
        val mono = "${p.fontName.ifBlank { "monospace" }}, monospace"
        val s = p.fontSizePt
        val bgRule = p.background?.let { " background: $it;" } ?: ""
        val css = """
            body { color: ${p.foreground}; font-family: sans-serif; font-size: ${pt(s)};
                   margin: 0 6px; padding: 4px 0;$bgRule }
            h1, h2, h3, h4, h5, h6 { color: ${p.foreground}; margin: 8px 0 4px 0; font-weight: bold; }
            h1 { font-size: ${pt(s + 5)}; } h2 { font-size: ${pt(s + 3)}; } h3 { font-size: ${pt(s + 1)}; }
            h4, h5, h6 { font-size: ${pt(s)}; }
            p { margin: 4px 0; }
            a { color: ${p.accent}; text-decoration: underline; }
            ul, ol { margin: 4px 0 4px 20px; padding: 0; }
            li { margin: 2px 0; }
            code { font-family: $mono; background: ${p.codeBackground};
                   padding: 0 3px; }
            pre { font-family: $mono; background: ${p.codeBackground};
                  border: 1px solid ${p.border}; margin: 6px 0; padding: 6px 8px; }
            pre code { background: transparent; padding: 0; }
            blockquote { margin: 6px 0; padding: 2px 0 2px 10px;
                         border-left: 3px solid ${p.accent}; color: ${p.muted}; }
            table { border: 1px solid ${p.border}; margin: 6px 0; }
            th, td { border: 1px solid ${p.border}; padding: 3px 7px; }
            th { background: ${p.codeBackground}; font-weight: bold; text-align: left; }
            hr { border: 0; border-top: 1px solid ${p.border}; margin: 8px 0; }
        """.trimIndent()
        return "<html><head><style>$css</style></head><body>$bodyHtml</body></html>"
    }

    /**
     * A CSS point length, rounded to a tenth and rendered without a trailing
     * `.0` — `13f` → `"13pt"`, `13.5f` → `"13.5pt"`. Uses `Float.toString`, which
     * is locale-independent, so a comma-decimal locale can't emit `13,5pt` (which
     * every CSS parser would drop).
     */
    internal fun pt(value: Float): String {
        val tenths = Math.round(value * 10f) / 10f
        val whole = tenths.toInt()
        return if (tenths == whole.toFloat()) "${whole}pt" else "${tenths}pt"
    }
}
