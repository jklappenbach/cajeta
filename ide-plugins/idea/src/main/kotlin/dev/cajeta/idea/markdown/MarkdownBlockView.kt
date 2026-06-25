package dev.cajeta.idea.markdown

import com.intellij.ui.jcef.JBCefApp
import dev.cajeta.idea.settings.CajetaSettings
import java.awt.Graphics2D

/**
 * A renderable markdown block, decoupled from the editor fold region so the
 * rendering surface can be swapped (Swing or JCEF) without touching the fold
 * builder / renderer. Measures and paints synchronously; an async surface (JCEF)
 * paints a fallback until its frame is ready and then calls the [bindRepaint]
 * callback to ask the fold region to re-measure and repaint.
 */
interface MarkdownBlockView {
    /** Preferred height to lay out the block at [width] px, never below [minHeight]. */
    fun heightForWidth(width: Int, minHeight: Int): Int

    /** Paint the block at ([x],[y]) into [width]x[height] px of [g]. */
    fun paint(g: Graphics2D, x: Double, y: Double, width: Int, height: Int)

    /** Supply a callback the view invokes when an asynchronously-produced render
     *  becomes available, so the fold region re-measures + repaints. Default
     *  no-op (synchronous surfaces never need it). */
    fun bindRepaint(repaint: () -> Unit) {}
}

/**
 * Chooses the rendering surface for a markdown block (spec: markdown JCEF
 * prototype). Default is the synchronous [SwingMarkdownBlockView]. When the user
 * opts into JCEF (Settings ▸ Cajeta) *and* the environment supports it, returns
 * the Chromium-backed [JcefMarkdownBlockView], which renders the same
 * [MarkdownHtmlTheme] HTML with full CSS fidelity (tables, code highlighting,
 * images) and falls back to Swing until/unless its frame is ready.
 */
object MarkdownBlockViewFactory {

    fun jcefRenderingAvailable(): Boolean =
        runCatching { JBCefApp.isSupported() }.getOrDefault(false)

    fun create(html: String): MarkdownBlockView {
        val wantJcef = CajetaSettings.instance.markdownRenderSurface == CajetaSettings.MARKDOWN_SURFACE_JCEF
        return if (wantJcef && jcefRenderingAvailable()) JcefMarkdownBlockView(html)
        else SwingMarkdownBlockView(html)
    }
}
