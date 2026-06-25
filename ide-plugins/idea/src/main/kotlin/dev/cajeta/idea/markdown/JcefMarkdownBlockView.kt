package dev.cajeta.idea.markdown

import com.intellij.openapi.editor.Editor
import java.awt.Graphics2D
import java.awt.image.BufferedImage

/**
 * A markdown block rendered by JCEF (Chromium) for full CSS fidelity. JCEF is
 * asynchronous and cannot paint into the fold region's `Graphics2D`, so this
 * view renders the themed HTML to a [BufferedImage] **once** (off-screen, then
 * disposes the browser — no live browser is retained per fold, so nothing
 * leaks), and paints that image thereafter. Until the image is ready — and if
 * JCEF rendering fails for any reason — it paints the synchronous
 * [SwingMarkdownBlockView] fallback, so the block is never blank.
 *
 * Experimental: selected only when the user opts into the JCEF surface and the
 * environment supports it (see [MarkdownBlockViewFactory]).
 */
class JcefMarkdownBlockView(private val html: String) : MarkdownBlockView {

    private val fallback = SwingMarkdownBlockView(html)

    @Volatile private var image: BufferedImage? = null
    /** The (width, fontSize) the current image/in-flight render is for; a change
     *  (e.g. zoom) invalidates it and triggers a re-render. */
    private var renderKey: Pair<Int, Int>? = null
    private var repaint: (() -> Unit)? = null

    override fun bindRepaint(repaint: () -> Unit) { this.repaint = repaint }

    override fun heightForWidth(editor: Editor, width: Int, minHeight: Int): Int {
        val img = image
        return img?.height?.coerceAtLeast(minHeight)
            ?: fallback.heightForWidth(editor, width, minHeight)
    }

    override fun paint(editor: Editor, g: Graphics2D, x: Double, y: Double, width: Int, height: Int) {
        ensureRenderStarted(editor, width)
        val img = image
        if (img == null) {
            fallback.paint(editor, g, x, y, width, height)   // until the frame is ready
            return
        }
        val gc = g.create() as Graphics2D
        try {
            gc.translate(x, y)
            gc.drawImage(img, 0, 0, null)
        } finally {
            gc.dispose()
        }
    }

    /** (Re)start the one-shot off-screen render when the width or zoom changes.
     *  JCEF has no editor behind it, so theme with an opaque background. */
    private fun ensureRenderStarted(editor: Editor, width: Int) {
        if (width <= 0) return
        val key = width to editor.colorsScheme.editorFontSize
        if (key == renderKey) return
        renderKey = key
        image = null   // fall back to Swing until the new image arrives
        val doc = MarkdownHtmlTheme.wrap(html, EditorMarkdownPalette.forEditor(editor, withBackground = true))
        JcefHtmlImageRenderer.render(doc, width) { rendered ->
            if (renderKey == key) {        // ignore a stale render if zoom moved on
                image = rendered
                repaint?.invoke()          // ask the fold region to re-measure + repaint
            }
        }
    }
}
