package dev.cajeta.idea.markdown

import com.intellij.openapi.editor.CustomFoldRegion
import com.intellij.openapi.editor.CustomFoldRegionRenderer
import com.intellij.openapi.editor.markup.TextAttributes
import com.intellij.util.ui.UIUtil
import java.awt.Color
import java.awt.Graphics2D
import java.awt.geom.Rectangle2D

/**
 * Paints rendered markdown in place of a folded comment region. The actual
 * rendering is delegated to a [MarkdownBlockView] (chosen by
 * [MarkdownBlockViewFactory]) so the surface can evolve — Swing today, a
 * full-fidelity JCEF view later — without changing the fold plumbing. This class
 * owns only the fold-region concerns: width binding to the editor and a subtle
 * background tint that signals "this is a rendered region".
 */
class MarkdownFoldRenderer(markdown: String) : CustomFoldRegionRenderer {

    private val html: String = MarkdownEngineRegistry.getInstance().active().renderToHtml(markdown)
    private val view: MarkdownBlockView = MarkdownBlockViewFactory.create(html)

    override fun calcWidthInPixels(region: CustomFoldRegion): Int =
        region.editor.contentComponent.width.coerceAtLeast(300)

    override fun calcHeightInPixels(region: CustomFoldRegion): Int =
        view.heightForWidth(calcWidthInPixels(region), region.editor.lineHeight)

    override fun paint(
        region: CustomFoldRegion,
        g: Graphics2D,
        targetRegion: Rectangle2D,
        textAttributes: TextAttributes,
    ) {
        val width = targetRegion.width.toInt().coerceAtLeast(10)
        val height = targetRegion.height.toInt().coerceAtLeast(region.editor.lineHeight)
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
        view.paint(g, targetRegion.x, targetRegion.y, width, height)
    }

    private fun backgroundTint(): Color? =
        if (UIUtil.isUnderDarcula()) Color(255, 255, 255, 12) else Color(0, 0, 0, 8)
}
