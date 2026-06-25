package dev.cajeta.idea.markdown

import java.awt.Dimension
import java.awt.Graphics2D
import javax.swing.JEditorPane
import javax.swing.text.html.HTMLEditorKit

/**
 * Renders a markdown block with Swing's [JEditorPane] + [HTMLEditorKit] over the
 * themed [MarkdownHtmlTheme] HTML. Synchronous and always available (works
 * headless), it is the default surface and the fallback for the JCEF view. The
 * pane is built lazily and cached per width. CSS fidelity is bounded by
 * HTMLEditorKit (a partial CSS engine); the theme targets what it supports.
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
        // Transparent body so the fold tint shows through (no background color).
        val styled = MarkdownHtmlTheme.wrap(bodyHtml, EditorMarkdownPalette.current(withBackground = false))
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
}
