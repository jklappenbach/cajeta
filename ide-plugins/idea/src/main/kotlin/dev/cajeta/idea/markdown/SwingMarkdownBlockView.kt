package dev.cajeta.idea.markdown

import com.intellij.openapi.editor.colors.EditorColorsManager
import java.awt.Color
import java.awt.Dimension
import java.awt.Graphics2D
import javax.swing.JEditorPane
import javax.swing.text.html.HTMLEditorKit

/**
 * Renders a markdown block with Swing's [JEditorPane] + [HTMLEditorKit] over the
 * themed [MarkdownHtmlTheme] HTML. Synchronous and always available (works
 * headless), it is the default surface and the fallback for the planned JCEF
 * view. The pane is built lazily and cached per width. CSS fidelity is bounded
 * by HTMLEditorKit (a partial CSS engine); the theme targets what it supports.
 */
class SwingMarkdownBlockView(private val bodyHtml: String) : MarkdownBlockView {

    private var pane: JEditorPane? = null
    private var paneWidth: Int = -1

    override fun heightForWidth(width: Int, minHeight: Int): Int {
        val pane = ensurePane(width)
        pane.size = Dimension(width, Short.MAX_VALUE.toInt())
        return pane.preferredSize.height.coerceAtLeast(minHeight) + 4
    }

    override fun paint(g: Graphics2D, x: Double, y: Double, width: Int, height: Int) {
        val pane = ensurePane(width)
        pane.setBounds(0, 0, width, height)
        val gc = g.create() as Graphics2D
        try {
            gc.translate(x, y)
            pane.paint(gc)
        } finally {
            gc.dispose()
        }
    }

    private fun ensurePane(width: Int): JEditorPane {
        val existing = pane
        if (existing != null && paneWidth == width) return existing
        val styled = MarkdownHtmlTheme.wrap(bodyHtml, editorPalette())
        val newPane = JEditorPane().apply {
            contentType = "text/html"
            editorKit = HTMLEditorKit()
            isEditable = false
            isOpaque = false
            border = null
            text = styled
            size = Dimension(width, Short.MAX_VALUE.toInt())
        }
        pane = newPane
        paneWidth = width
        return newPane
    }

    /** Map the active editor color scheme to a theme palette so the block tracks
     *  the editor's look (dark/light, font) rather than looking foreign. */
    private fun editorPalette(): MarkdownHtmlTheme.Palette {
        val scheme = EditorColorsManager.getInstance().globalScheme
        val fg = scheme.defaultForeground
        val bg = scheme.defaultBackground
        val dark = isDark(bg)   // from the editor bg luminance; non-deprecated, theme-accurate
        return MarkdownHtmlTheme.Palette(
            foreground = fg.toHex(),
            muted = blend(fg, bg, 0.45).toHex(),
            accent = (if (dark) Color(0x4E, 0xA1, 0xFF) else Color(0x2F, 0x6F, 0xDB)).toHex(),
            codeBackground = blend(bg, fg, 0.08).toHex(),
            border = blend(bg, fg, 0.28).toHex(),
            fontName = scheme.editorFontName,
            fontSizePt = scheme.editorFontSize,
        )
    }

    private fun isDark(c: Color): Boolean = (0.299 * c.red + 0.587 * c.green + 0.114 * c.blue) < 128

    /** Linear blend: [ratio] of [b] mixed into [a]. */
    private fun blend(a: Color, b: Color, ratio: Double): Color {
        val r = (a.red * (1 - ratio) + b.red * ratio).toInt()
        val g = (a.green * (1 - ratio) + b.green * ratio).toInt()
        val bl = (a.blue * (1 - ratio) + b.blue * ratio).toInt()
        return Color(r.coerceIn(0, 255), g.coerceIn(0, 255), bl.coerceIn(0, 255))
    }

    private fun Color.toHex(): String = "#%02x%02x%02x".format(red, green, blue)
}
