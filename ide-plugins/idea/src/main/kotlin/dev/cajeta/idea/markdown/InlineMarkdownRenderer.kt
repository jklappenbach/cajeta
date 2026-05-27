package dev.cajeta.idea.markdown

import com.intellij.openapi.editor.EditorCustomElementRenderer
import com.intellij.openapi.editor.Inlay
import com.intellij.openapi.editor.colors.EditorColorsManager
import com.intellij.openapi.editor.markup.TextAttributes
import com.intellij.util.ui.UIUtil
import java.awt.Color
import java.awt.Font
import java.awt.Graphics2D
import java.awt.RenderingHints
import java.awt.geom.Rectangle2D

/**
 * Renders simple inline markdown for trailing comments using direct
 * Graphics2D text drawing. Limited to one line of vertical space and
 * to the four inline markdown features that matter in trailing
 * comments: **bold**, _italic_ (or *italic*), `code`, and plain text.
 *
 * Block-level markdown (headings, lists, code blocks) is rendered as
 * plain text in this inline view — the doc-block-above-code path
 * handles those richly. Switching to JEditorPane here was unreliable
 * because an offscreen pane returns preferredSize.width = 0 (UI not
 * realized), making the inlay invisible.
 */
class InlineMarkdownRenderer(markdown: String) : EditorCustomElementRenderer {

    private val segments: List<Segment> = parseInline(markdown)

    private data class Segment(val text: String, val style: Style)
    private enum class Style { NORMAL, BOLD, ITALIC, CODE }

    override fun calcWidthInPixels(inlay: Inlay<*>): Int {
        val baseFont = baseFont(inlay)
        val codeFont = codeFont(inlay)
        // Component.getFontMetrics(Font) works even when the component
        // isn't yet realized in a Swing hierarchy — unlike using the
        // component's Graphics, which can be null at first paint.
        val component = inlay.editor.contentComponent
        val total = segments.sumOf { seg ->
            val font = fontFor(seg.style, baseFont, codeFont)
            component.getFontMetrics(font).stringWidth(seg.text)
        }
        val width = (PADDING_HORIZONTAL * 2 + total).coerceAtLeast(20)
        return width.coerceAtMost(MAX_INLINE_WIDTH)
    }

    override fun paint(
        inlay: Inlay<*>,
        g: Graphics2D,
        targetRegion: Rectangle2D,
        textAttributes: TextAttributes,
    ) {
        val width = targetRegion.width.toInt()
        val height = targetRegion.height.toInt()
        val gCopy = g.create() as Graphics2D
        try {
            gCopy.translate(targetRegion.x, targetRegion.y)
            gCopy.setRenderingHint(
                RenderingHints.KEY_TEXT_ANTIALIASING,
                RenderingHints.VALUE_TEXT_ANTIALIAS_ON,
            )

            val bg = backgroundTint()
            if (bg != null) {
                gCopy.color = bg
                gCopy.fillRect(0, 0, width, height)
            }

            val baseFont = baseFont(inlay)
            val codeFont = codeFont(inlay)
            val fg = inlay.editor.colorsScheme.defaultForeground
            gCopy.color = fg

            // Baseline: text rendered with FontMetrics ascent below the
            // top of the region, mid-vertically.
            val fmBase = gCopy.getFontMetrics(baseFont)
            val y = (height + fmBase.ascent - fmBase.descent) / 2
            var x = PADDING_HORIZONTAL
            for (seg in segments) {
                val font = fontFor(seg.style, baseFont, codeFont)
                gCopy.font = font
                val fm = gCopy.getFontMetrics(font)
                val segWidth = fm.stringWidth(seg.text)
                if (seg.style == Style.CODE) {
                    val codeBg = codeBgTint()
                    if (codeBg != null) {
                        gCopy.color = codeBg
                        gCopy.fillRect(x - 1, y - fm.ascent, segWidth + 2, fm.height)
                        gCopy.color = fg
                    }
                }
                gCopy.drawString(seg.text, x, y)
                x += segWidth
                if (x > width - PADDING_HORIZONTAL) break  // overflow guard
            }
        } finally {
            gCopy.dispose()
        }
    }

    private fun baseFont(inlay: Inlay<*>): Font {
        val scheme = inlay.editor.colorsScheme
        return Font(scheme.editorFontName, Font.PLAIN, scheme.editorFontSize)
    }

    private fun codeFont(inlay: Inlay<*>): Font {
        val scheme = inlay.editor.colorsScheme
        // Use the editor's mono font, same as the surrounding code.
        return Font(scheme.editorFontName, Font.PLAIN, scheme.editorFontSize)
    }

    private fun fontFor(style: Style, base: Font, code: Font): Font = when (style) {
        Style.NORMAL -> base
        Style.BOLD -> base.deriveFont(Font.BOLD)
        Style.ITALIC -> base.deriveFont(Font.ITALIC)
        Style.CODE -> code
    }

    private fun estimateWidth(font: Font): Int {
        // Coarse fallback when no Graphics is available — should be
        // rare in practice.
        val charWidth = font.size / 2
        return (segments.sumOf { it.text.length } * charWidth + PADDING_HORIZONTAL * 2)
            .coerceAtMost(MAX_INLINE_WIDTH)
    }

    private fun backgroundTint(): Color? =
        if (UIUtil.isUnderDarcula()) Color(255, 255, 255, 12) else Color(0, 0, 0, 8)

    private fun codeBgTint(): Color? =
        if (UIUtil.isUnderDarcula()) Color(255, 255, 255, 18) else Color(0, 0, 0, 14)

    companion object {
        private const val MAX_INLINE_WIDTH = 600
        private const val PADDING_HORIZONTAL = 6

        /**
         * Parses inline markdown into runs of styled segments. Handles:
         *   - `**bold**`
         *   - `_italic_` and `*italic*`
         *   - `` `code` ``
         *
         * Anything not matched falls through as plain text. Nesting is
         * not supported (so `**_bold italic_**` becomes plain `_bold
         * italic_` inside bold — that's acceptable for trailing
         * comments).
         */
        private fun parseInline(text: String): List<Segment> {
            val out = mutableListOf<Segment>()
            var i = 0
            val n = text.length
            val plain = StringBuilder()

            fun flushPlain() {
                if (plain.isNotEmpty()) {
                    out += Segment(plain.toString(), Style.NORMAL)
                    plain.clear()
                }
            }

            while (i < n) {
                val c = text[i]
                // **bold**
                if (c == '*' && i + 1 < n && text[i + 1] == '*') {
                    val end = text.indexOf("**", startIndex = i + 2)
                    if (end > i + 2) {
                        flushPlain()
                        out += Segment(text.substring(i + 2, end), Style.BOLD)
                        i = end + 2
                        continue
                    }
                }
                // `code`
                if (c == '`') {
                    val end = text.indexOf('`', startIndex = i + 1)
                    if (end > i + 1) {
                        flushPlain()
                        out += Segment(text.substring(i + 1, end), Style.CODE)
                        i = end + 1
                        continue
                    }
                }
                // *italic* or _italic_ — require non-whitespace neighbor
                // on at least one side to avoid eating bullet asterisks.
                if ((c == '*' || c == '_') && i + 1 < n && !text[i + 1].isWhitespace()) {
                    val end = text.indexOf(c, startIndex = i + 1)
                    if (end > i + 1 && (end == n - 1 || !text[end - 1].isWhitespace())) {
                        flushPlain()
                        out += Segment(text.substring(i + 1, end), Style.ITALIC)
                        i = end + 1
                        continue
                    }
                }
                plain.append(c)
                i++
            }
            flushPlain()
            return out
        }
    }
}
