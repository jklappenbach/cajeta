package dev.cajeta.idea.markdown

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
    @Volatile private var started = false
    private var repaint: (() -> Unit)? = null

    override fun bindRepaint(repaint: () -> Unit) { this.repaint = repaint }

    override fun heightForWidth(width: Int, minHeight: Int): Int {
        val img = image
        return img?.height?.coerceAtLeast(minHeight)
            ?: fallback.heightForWidth(width, minHeight)
    }

    override fun paint(g: Graphics2D, x: Double, y: Double, width: Int, height: Int) {
        ensureRenderStarted(width)
        val img = image
        if (img == null) {
            fallback.paint(g, x, y, width, height)
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

    /** Kick off the one-shot off-screen render the first time we know our width.
     *  JCEF has no editor behind it, so theme with an opaque background. */
    private fun ensureRenderStarted(width: Int) {
        if (started || width <= 0) return
        started = true
        val doc = MarkdownHtmlTheme.wrap(html, EditorMarkdownPalette.current(withBackground = true))
        JcefHtmlImageRenderer.render(doc, width) { rendered ->
            image = rendered
            repaint?.invoke()   // ask the fold region to re-measure + repaint
        }
    }
}
