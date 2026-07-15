package dev.cajeta.idea.markdown

import com.intellij.openapi.editor.Editor
import com.intellij.openapi.editor.colors.EditorColors
import java.awt.Dimension
import java.awt.Graphics2D
import java.awt.geom.Point2D
import javax.swing.JEditorPane
import javax.swing.text.html.HTMLEditorKit

/**
 * Renders a markdown block with Swing's [JEditorPane] + [HTMLEditorKit] over the
 * themed [MarkdownHtmlTheme] HTML. Synchronous and always available (works
 * headless), it is the default surface and the fallback for the JCEF view. The
 * pane is built lazily and cached per (width, font size). CSS fidelity is bounded
 * by HTMLEditorKit (a partial CSS engine); the theme targets what it supports.
 *
 * This surface also backs **text selection** over the rendered block: because the
 * pane lays out real text (rather than baking it into an image), a point can be
 * mapped back to a document offset and a selection highlighted. The pane is never
 * added to a Swing hierarchy — it is laid out off-screen and painted straight
 * into the fold region's `Graphics2D` — so selection is driven entirely through
 * the API here rather than by the pane's own mouse handling.
 */
class SwingMarkdownBlockView(private val bodyHtml: String) : MarkdownBlockView {

    private var pane: JEditorPane? = null
    private var paneWidth: Int = -1
    private var paneFontSize: Float = -1f

    override val supportsSelection: Boolean get() = true

    override fun heightForWidth(editor: Editor, width: Int, minHeight: Int): Int {
        val pane = ensurePane(editor, width)
        pane.size = Dimension(width, Short.MAX_VALUE.toInt())
        return pane.preferredSize.height.coerceAtLeast(minHeight) + 4
    }

    override fun paint(editor: Editor, g: Graphics2D, x: Double, y: Double, width: Int, height: Int) {
        val pane = ensurePane(editor, width)
        pane.setBounds(0, 0, width, height)
        val gc = g.create() as Graphics2D
        try {
            gc.translate(x, y)
            pane.paint(gc)
        } finally {
            gc.dispose()
        }
    }

    /**
     * Maps a point in block-local pixels to an offset in the rendered document.
     * The pane must be laid out at the same width it is painted at, or the
     * mapping lands on the wrong line.
     */
    override fun offsetAt(editor: Editor, width: Int, px: Int, py: Int): Int {
        val pane = ensurePane(editor, width)
        pane.size = Dimension(width, Short.MAX_VALUE.toInt())
        // Force a layout pass; viewToModel2D on an unlaid-out root view returns 0.
        pane.preferredSize
        return pane.viewToModel2D(Point2D.Double(px.toDouble(), py.toDouble()))
    }

    override fun setSelection(from: Int, to: Int) {
        val pane = pane ?: return
        val lo = minOf(from, to).coerceIn(0, pane.document.length)
        val hi = maxOf(from, to).coerceIn(0, pane.document.length)
        // The pane never receives focus (it isn't in a Swing hierarchy), and
        // DefaultCaret only shows a selection once focused — so force it on, or
        // the highlight never paints.
        pane.caret.isSelectionVisible = true
        pane.select(lo, hi)
    }

    override fun clearSelection() {
        val pane = pane ?: return
        pane.select(0, 0)
        pane.caret.isSelectionVisible = false
    }

    /** The rendered (plain) text under the current selection — what Copy yields. */
    override fun selectedText(): String? = pane?.selectedText?.takeIf { it.isNotEmpty() }

    private fun ensurePane(editor: Editor, width: Int): JEditorPane {
        val fontSize = editor.colorsScheme.editorFontSize2D   // zoom-aware, fractional
        val existing = pane
        if (existing != null && paneWidth == width && paneFontSize == fontSize) return existing
        // Transparent body so the fold tint shows through (no background color).
        val styled = MarkdownHtmlTheme.wrap(bodyHtml, EditorMarkdownPalette.forEditor(editor, withBackground = false))
        val newPane = JEditorPane().apply {
            contentType = "text/html"
            editorKit = HTMLEditorKit()
            isEditable = false
            isOpaque = false
            border = null
            text = styled
            // Match the editor's own selection color so a highlight inside a
            // rendered block looks like a highlight anywhere else in the file.
            editor.colorsScheme.getColor(EditorColors.SELECTION_BACKGROUND_COLOR)?.let { selectionColor = it }
            editor.colorsScheme.getColor(EditorColors.SELECTION_FOREGROUND_COLOR)?.let { selectedTextColor = it }
            size = Dimension(width, Short.MAX_VALUE.toInt())
        }
        pane = newPane
        paneWidth = width
        paneFontSize = fontSize
        return newPane
    }
}
