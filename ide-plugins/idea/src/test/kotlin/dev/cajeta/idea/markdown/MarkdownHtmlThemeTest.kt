package dev.cajeta.idea.markdown

import org.junit.Assert.assertEquals
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
        codeBackground = "#202020", border = "#404040", fontName = "JetBrains Mono", fontSizePt = 13f,
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

    /**
     * Zoom sets a *fractional* font size. It has to survive into the CSS: rounding
     * it to an Int made every sub-point zoom step render identically, which is the
     * bug this guards. Swing's HTMLEditorKit parses a fractional `pt` length, so
     * emitting one is safe on both surfaces.
     */
    @Test
    fun inlineAndFencedCodeCarryTheEditorFontSize() {
        // Bug: code/pre had no font-size, so Swing rendered them at a fixed
        // default that didn't scale with the surrounding comment text.
        val doc = MarkdownHtmlTheme.wrap("<p><code>x</code></p>", palette.copy(fontSizePt = 13.5f))
        assertTrue("inline code carries the editor size",
            Regex("""\bcode\s*\{[^}]*font-size:\s*13\.5pt""").containsMatchIn(doc))
        assertTrue("fenced code carries the editor size",
            Regex("""\bpre\s*\{[^}]*font-size:\s*13\.5pt""").containsMatchIn(doc))
    }

    @Test
    fun carriesFractionalFontSizeIntoCss() {
        val doc = MarkdownHtmlTheme.wrap("<p>hi</p>", palette.copy(fontSizePt = 13.5f))
        assertTrue("fractional body size emitted", doc.contains("font-size: 13.5pt"))
        // Headings are relative to it and stay fractional.
        assertTrue("h3 = s+1", doc.contains("14.5pt"))
        assertTrue("h1 = s+5", doc.contains("18.5pt"))
    }

    @Test
    fun formatsPointLengthsWithoutTrailingZeroOrLocaleComma() {
        assertEquals("13pt", MarkdownHtmlTheme.pt(13f))
        assertEquals("13pt", MarkdownHtmlTheme.pt(13.0f))
        assertEquals("13.5pt", MarkdownHtmlTheme.pt(13.5f))
        // Rounded to a tenth — no 13.333333pt in the stylesheet.
        assertEquals("13.3pt", MarkdownHtmlTheme.pt(13.33333f))
    }
}
