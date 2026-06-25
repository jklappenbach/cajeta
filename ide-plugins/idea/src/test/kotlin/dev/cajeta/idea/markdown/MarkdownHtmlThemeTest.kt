package dev.cajeta.idea.markdown

import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The themed-HTML contract (the single styled document both the Swing fold view
 * and a future JCEF view render): the body is embedded verbatim and the GFM
 * constructs that matter in code comments — tables, fenced/inline code,
 * blockquotes — all get styled, with the caller's palette colors threaded in.
 */
class MarkdownHtmlThemeTest {

    private val palette = MarkdownHtmlTheme.Palette(
        foreground = "#e0e0e0", muted = "#909090", accent = "#5a9cff",
        codeBackground = "#202020", border = "#404040", fontName = "JetBrains Mono", fontSizePt = 13,
    )

    @Test
    fun embedsBodyAndStylesGfmConstructs() {
        val body = "<h2>Title</h2><table><tr><td>a</td></tr></table><pre><code>x</code></pre><blockquote>q</blockquote>"
        val doc = MarkdownHtmlTheme.wrap(body, palette)

        assertTrue("body embedded verbatim", doc.contains(body))
        assertTrue("is a full html doc", doc.startsWith("<html>") && doc.contains("<style>"))
        // GFM constructs are styled
        assertTrue("tables get borders", Regex("""\bth,\s*td\s*\{[^}]*border:""").containsMatchIn(doc))
        assertTrue("fenced code styled", Regex("""\bpre\s*\{[^}]*background:""").containsMatchIn(doc))
        assertTrue("blockquote styled", Regex("""\bblockquote\s*\{[^}]*border-left:""").containsMatchIn(doc))
    }

    @Test
    fun threadsPaletteColorsAndFontIntoCss() {
        val doc = MarkdownHtmlTheme.wrap("<p>hi</p>", palette)
        assertTrue("foreground used", doc.contains("#e0e0e0"))
        assertTrue("accent used for links", doc.contains("#5a9cff"))
        assertTrue("code background used", doc.contains("#202020"))
        assertTrue("editor font used for code", doc.contains("JetBrains Mono"))
        assertTrue("font size threaded", doc.contains("13pt"))
    }
}
