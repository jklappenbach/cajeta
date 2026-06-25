package dev.cajeta.idea.markdown

import com.intellij.ui.jcef.JBCefApp
import java.awt.Graphics2D

/**
 * A renderable markdown block, decoupled from the editor fold region so the
 * rendering surface can be swapped (Swing today, JCEF later) without touching
 * the fold builder / renderer. Measures and paints synchronously.
 */
interface MarkdownBlockView {
    /** Preferred height to lay out the block at [width] px, never below [minHeight]. */
    fun heightForWidth(width: Int, minHeight: Int): Int

    /** Paint the block at ([x],[y]) into [width]x[height] px of [g]. */
    fun paint(g: Graphics2D, x: Double, y: Double, width: Int, height: Int)
}

/**
 * Chooses the rendering surface for a markdown block. Today it always returns
 * the synchronous [SwingMarkdownBlockView] (works headless, no async frames).
 *
 * [jcefRenderingAvailable] reports whether the environment could host a
 * full-fidelity JCEF (Chromium) surface — the planned higher-fidelity view that
 * renders the same [MarkdownHtmlTheme] HTML with real CSS (tables, code
 * highlighting, images). That view requires off-screen rendering wired into the
 * fold region and can only be validated in a running IDE, so it is staged behind
 * this seam rather than shipped untested; [create] is the exact drop-in point.
 */
object MarkdownBlockViewFactory {

    fun jcefRenderingAvailable(): Boolean =
        runCatching { JBCefApp.isSupported() }.getOrDefault(false)

    fun create(html: String): MarkdownBlockView = SwingMarkdownBlockView(html)
}
