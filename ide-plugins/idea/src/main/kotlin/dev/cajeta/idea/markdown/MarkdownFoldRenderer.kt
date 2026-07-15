package dev.cajeta.idea.markdown

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.editor.CustomFoldRegion
import com.intellij.openapi.editor.CustomFoldRegionRenderer
import com.intellij.openapi.editor.Editor
import com.intellij.openapi.editor.ex.util.EditorUtil
import com.intellij.openapi.editor.markup.TextAttributes
import com.intellij.util.ui.UIUtil
import java.awt.Color
import java.awt.Font
import java.awt.Graphics2D
import java.awt.geom.Rectangle2D

/**
 * Paints rendered markdown in place of a folded comment region. The actual
 * rendering is delegated to a [MarkdownBlockView] (chosen by
 * [MarkdownBlockViewFactory]) so the surface can evolve (Swing / JCEF) without
 * changing the fold plumbing. This class owns the fold-region concerns: the
 * indent (so the block sits at the indentation of the code it documents), width
 * binding (to the right-margin guide), zoom-driven re-measure, and a subtle
 * background tint that signals "this is a rendered region".
 *
 * [indentColumns] is the column the source comment starts at. A `CustomFoldRegion`
 * always begins painting at the left edge of the text area, so the indent is
 * applied here — as a paint-time offset and a matching width reservation — rather
 * than by the folding model. The rendered markdown itself is dedented by
 * `stripCommentMarkers`, so this is purely presentational.
 */
class MarkdownFoldRenderer(
    markdown: String,
    private val indentColumns: Int = 0,
) : CustomFoldRegionRenderer {

    private val html: String = MarkdownEngineRegistry.getInstance().active().renderToHtml(markdown)
    private val view: MarkdownBlockView = MarkdownBlockViewFactory.create(html)
    private var repaintBound = false
    private var lastFontSize = -1f

    /** Bind the async-render repaint to this region once we have one: re-measure
     *  + repaint when an async surface (JCEF) finishes. Idempotent. */
    private fun bindRepaint(region: CustomFoldRegion) {
        if (repaintBound) return
        repaintBound = true
        view.bindRepaint { scheduleUpdate(region) }
    }

    private fun scheduleUpdate(region: CustomFoldRegion) {
        ApplicationManager.getApplication().invokeLater {
            if (region.isValid) region.update()
        }
    }

    /** Pixels the block is inset from the left text edge, matching the code's scope. */
    fun indentPx(editor: Editor): Int {
        if (indentColumns <= 0) return 0
        val spaceWidth = EditorUtil.getSpaceWidth(Font.PLAIN, editor)
        return if (spaceWidth > 0) indentColumns * spaceWidth else 0
    }

    /**
     * Width of the rendered block itself — the span from the indent to the
     * editor's right-margin guide (the "suggested code width" marker), so the
     * block lines up with where code wraps and leaves a margin on the right
     * rather than spanning the whole window. Falls back to the full content width
     * when no margin is configured. Uses the editor's current space width, so it
     * scales with zoom.
     */
    fun bodyWidth(region: CustomFoldRegion): Int {
        val editor = region.editor
        val content = editor.contentComponent.width
        val marginCols = editor.settings.getRightMargin(editor.project)
        val spaceWidth = EditorUtil.getSpaceWidth(Font.PLAIN, editor)
        val marginPx = if (marginCols > 0 && spaceWidth > 0) marginCols * spaceWidth else Int.MAX_VALUE
        val rightEdge = marginPx.coerceAtMost(content)
        return (rightEdge - indentPx(editor)).coerceAtLeast(MIN_WIDTH)
    }

    /** The block view, when it can back a text selection (Swing, not JCEF). */
    fun selectableView(): MarkdownBlockView? = view.takeIf { it.supportsSelection }

    /** Total reserved width = the indent plus the block. */
    override fun calcWidthInPixels(region: CustomFoldRegion): Int =
        indentPx(region.editor) + bodyWidth(region)

    override fun calcHeightInPixels(region: CustomFoldRegion): Int =
        view.heightForWidth(region.editor, bodyWidth(region), region.editor.lineHeight)

    override fun paint(
        region: CustomFoldRegion,
        g: Graphics2D,
        targetRegion: Rectangle2D,
        textAttributes: TextAttributes,
    ) {
        bindRepaint(region)
        val editor = region.editor

        // On a zoom change the fold's height must be recomputed (line height +
        // content both changed); ask the region to re-measure once per change.
        // Fractional: sub-point zoom steps must not round away to "unchanged".
        val fontSize = editor.colorsScheme.editorFontSize2D
        if (lastFontSize != -1f && lastFontSize != fontSize) scheduleUpdate(region)
        lastFontSize = fontSize

        val indent = indentPx(editor)
        val width = (targetRegion.width.toInt() - indent).coerceAtLeast(10)
        val height = targetRegion.height.toInt().coerceAtLeast(editor.lineHeight)
        val originX = targetRegion.x + indent

        val gCopy = g.create() as Graphics2D
        try {
            gCopy.translate(originX, targetRegion.y)
            backgroundTint()?.let {
                gCopy.color = it
                gCopy.fillRect(0, 0, width, height)
            }
        } finally {
            gCopy.dispose()
        }
        view.paint(editor, g, originX, targetRegion.y, width, height)
    }

    private fun backgroundTint(): Color? =
        if (UIUtil.isUnderDarcula()) Color(255, 255, 255, 12) else Color(0, 0, 0, 8)

    private companion object {
        const val MIN_WIDTH = 300
    }
}
