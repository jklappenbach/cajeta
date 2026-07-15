package dev.cajeta.idea.markdown

import com.intellij.openapi.editor.Editor
import com.intellij.ui.jcef.JBCefApp
import dev.cajeta.idea.settings.CajetaSettings
import java.awt.Graphics2D

/**
 * A renderable markdown block, decoupled from the editor fold region so the
 * rendering surface can be swapped (Swing or JCEF) without touching the fold
 * builder / renderer. The [editor] is supplied on each call so the render tracks
 * its per-editor zoom (font size). An async surface (JCEF) paints a fallback
 * until its frame is ready and then calls the [bindRepaint] callback to ask the
 * fold region to re-measure and repaint.
 */
interface MarkdownBlockView {
    /** Preferred height to lay out the block at [width] px, never below [minHeight]. */
    fun heightForWidth(editor: Editor, width: Int, minHeight: Int): Int

    /** Paint the block at ([x],[y]) into [width]x[height] px of [g]. */
    fun paint(editor: Editor, g: Graphics2D, x: Double, y: Double, width: Int, height: Int)

    /** Supply a callback the view invokes when an asynchronously-produced render
     *  becomes available, so the fold region re-measures + repaints. Default
     *  no-op (synchronous surfaces never need it). */
    fun bindRepaint(repaint: () -> Unit) {}

    // ---- Text selection over the rendered block -------------------------------
    // Only a surface that lays out real text can map a pixel back to a character
    // and highlight a range. The Swing surface can; the JCEF surface renders to a
    // BufferedImage and has no text model to select against, so it declines here
    // and the selection controller leaves its blocks alone.

    /** Whether this surface can map points to offsets and highlight a range. */
    val supportsSelection: Boolean get() = false

    /** Offset in the rendered document at block-local pixel ([px], [py]); -1 if unsupported. */
    fun offsetAt(editor: Editor, width: Int, px: Int, py: Int): Int = -1

    /** Highlight [from]..[to] (either order) in the rendered document. */
    fun setSelection(from: Int, to: Int) {}

    /** Drop any highlight. */
    fun clearSelection() {}

    /** Plain text under the current selection, or null when nothing is selected. */
    fun selectedText(): String? = null
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
