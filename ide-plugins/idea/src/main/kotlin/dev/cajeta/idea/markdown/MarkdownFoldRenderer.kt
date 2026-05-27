package dev.cajeta.idea.markdown

import com.intellij.openapi.editor.CustomFoldRegion
import com.intellij.openapi.editor.CustomFoldRegionRenderer
import com.intellij.openapi.editor.colors.EditorColorsManager
import com.intellij.openapi.editor.markup.TextAttributes
import com.intellij.util.ui.UIUtil
import java.awt.Color
import java.awt.Dimension
import java.awt.Graphics2D
import java.awt.geom.Rectangle2D
import javax.swing.JEditorPane
import javax.swing.text.html.HTMLEditorKit

/**
 * Paints rendered markdown HTML in place of a folded comment region.
 * Uses Swing's JEditorPane with HTMLEditorKit for the render; the
 * pane is created lazily and cached per renderer instance.
 *
 * Width is bound to the editor's content-component width so that
 * markdown wrapping looks right at any IDE zoom level. Height is
 * derived from the pane's preferred size at that width.
 */
class MarkdownFoldRenderer(private val markdown: String) : CustomFoldRegionRenderer {

    private val html: String = MarkdownEngineRegistry.getInstance().active().renderToHtml(markdown)
    private var pane: JEditorPane? = null
    private var paneWidth: Int = -1

    override fun calcWidthInPixels(region: CustomFoldRegion): Int {
        val editor = region.editor
        val available = (editor.contentComponent.width).coerceAtLeast(300)
        return available
    }

    override fun calcHeightInPixels(region: CustomFoldRegion): Int {
        val width = calcWidthInPixels(region)
        val pane = ensurePane(width)
        // Force layout at the requested width and read the preferred
        // height. Add a small bottom padding for visual breathing room.
        pane.size = Dimension(width, Short.MAX_VALUE.toInt())
        val pref = pane.preferredSize
        return pref.height.coerceAtLeast(region.editor.lineHeight) + 4
    }

    override fun paint(
        region: CustomFoldRegion,
        g: Graphics2D,
        targetRegion: Rectangle2D,
        textAttributes: TextAttributes,
    ) {
        val width = targetRegion.width.toInt().coerceAtLeast(10)
        val height = targetRegion.height.toInt().coerceAtLeast(region.editor.lineHeight)
        val pane = ensurePane(width)
        pane.setBounds(0, 0, width, height)

        val gCopy = g.create() as Graphics2D
        try {
            gCopy.translate(targetRegion.x, targetRegion.y)
            // Background tint that hints "this is a rendered region"
            // without competing with the editor's color scheme.
            val bg = backgroundTint()
            if (bg != null) {
                gCopy.color = bg
                gCopy.fillRect(0, 0, width, height)
            }
            pane.paint(gCopy)
        } finally {
            gCopy.dispose()
        }
    }

    private fun ensurePane(width: Int): JEditorPane {
        val existing = pane
        if (existing != null && paneWidth == width) return existing

        val kit = HTMLEditorKit()
        val styled = wrapWithThemeCss(html)
        val newPane = JEditorPane().apply {
            contentType = "text/html"
            editorKit = kit
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

    private fun wrapWithThemeCss(bodyHtml: String): String {
        val scheme = EditorColorsManager.getInstance().globalScheme
        val fg = scheme.defaultForeground.toHex()
        val accent = scheme.getColor(com.intellij.openapi.editor.colors.EditorColors.GUTTER_BACKGROUND)?.toHex() ?: fg
        val fontFamily = scheme.editorFontName
        val fontSize = scheme.editorFontSize
        val mono = "${fontFamily.ifBlank { "monospace" }}, monospace"

        // Inline CSS keeps the rendered block visually consistent with
        // the editor's font and color scheme, so it doesn't look like
        // a foreign Web widget dropped into the editor.
        val css = """
            body { color: $fg; font-family: sans-serif; font-size: ${fontSize}pt;
                   margin: 0 6px; padding: 4px 0; }
            h1, h2, h3, h4 { color: $fg; margin: 6px 0 4px 0; }
            h1 { font-size: ${fontSize + 4}pt; }
            h2 { font-size: ${fontSize + 2}pt; }
            h3 { font-size: ${fontSize + 1}pt; }
            p { margin: 4px 0; }
            ul, ol { margin: 4px 0 4px 18px; padding: 0; }
            li { margin: 2px 0; }
            code, pre { font-family: $mono; }
            code { padding: 0 2px; }
            pre { margin: 4px 0; padding: 4px 6px; }
            a { color: $accent; }
        """.trimIndent()

        return "<html><head><style>$css</style></head><body>$bodyHtml</body></html>"
    }

    private fun backgroundTint(): Color? {
        return if (UIUtil.isUnderDarcula()) {
            Color(255, 255, 255, 12)
        } else {
            Color(0, 0, 0, 8)
        }
    }

    private fun Color.toHex(): String = "#%02x%02x%02x".format(red, green, blue)
}
