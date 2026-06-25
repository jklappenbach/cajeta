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
        val fontSizePt: Int,
        /** Body background. Null = transparent (Swing, painted over the fold tint);
         *  a color = opaque (JCEF, which has no transparent editor behind it). */
        val background: String? = null,
    )

    fun wrap(bodyHtml: String, p: Palette): String {
        val mono = "${p.fontName.ifBlank { "monospace" }}, monospace"
        val s = p.fontSizePt
        val bgRule = p.background?.let { " background: $it;" } ?: ""
        val css = """
            body { color: ${p.foreground}; font-family: sans-serif; font-size: ${s}pt;
                   margin: 0 6px; padding: 4px 0;$bgRule }
            h1, h2, h3, h4, h5, h6 { color: ${p.foreground}; margin: 8px 0 4px 0; font-weight: bold; }
            h1 { font-size: ${s + 5}pt; } h2 { font-size: ${s + 3}pt; } h3 { font-size: ${s + 1}pt; }
            h4, h5, h6 { font-size: ${s}pt; }
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
}
