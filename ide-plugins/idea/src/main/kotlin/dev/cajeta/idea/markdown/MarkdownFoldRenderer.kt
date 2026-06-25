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
 * changing the fold plumbing. This class owns the fold-region concerns: width
 * binding (to the right-margin guide), zoom-driven re-measure, and a subtle
 * background tint that signals "this is a rendered region".
 */
class MarkdownFoldRenderer(markdown: String) : CustomFoldRegionRenderer {

    private val html: String = MarkdownEngineRegistry.getInstance().active().renderToHtml(markdown)
    private val view: MarkdownBlockView = MarkdownBlockViewFactory.create(html)
    private var repaintBound = false
    private var lastFontSize = -1

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

    /**
     * Width = the editor's right-margin guide (the "suggested code width"
     * marker), so the rendered block lines up with where code wraps and leaves a
     * margin on the right rather than spanning the whole window. Falls back to the
     * full content width when no margin is configured. Uses the editor's current
     * space width, so it scales with zoom.
     */
    override fun calcWidthInPixels(region: CustomFoldRegion): Int {
        val editor = region.editor
        val content = editor.contentComponent.width
        val marginCols = editor.settings.getRightMargin(editor.project)
        val spaceWidth = EditorUtil.getSpaceWidth(Font.PLAIN, editor)
        val marginPx = if (marginCols > 0 && spaceWidth > 0) marginCols * spaceWidth else Int.MAX_VALUE
        return marginPx.coerceAtMost(content).coerceAtLeast(MIN_WIDTH)
    }

    override fun calcHeightInPixels(region: CustomFoldRegion): Int =
        view.heightForWidth(region.editor, calcWidthInPixels(region), region.editor.lineHeight)

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
        val fontSize = editor.colorsScheme.editorFontSize
        if (lastFontSize != -1 && lastFontSize != fontSize) scheduleUpdate(region)
        lastFontSize = fontSize

        val width = targetRegion.width.toInt().coerceAtLeast(10)
        val height = targetRegion.height.toInt().coerceAtLeast(editor.lineHeight)
        val gCopy = g.create() as Graphics2D
        try {
            gCopy.translate(targetRegion.x, targetRegion.y)
            backgroundTint()?.let {
                gCopy.color = it
                gCopy.fillRect(0, 0, width, height)
            }
        } finally {
            gCopy.dispose()
        }
        view.paint(editor, g, targetRegion.x, targetRegion.y, width, height)
    }

    private fun backgroundTint(): Color? =
        if (UIUtil.isUnderDarcula()) Color(255, 255, 255, 12) else Color(0, 0, 0, 8)

    private companion object {
        const val MIN_WIDTH = 300
    }
}
