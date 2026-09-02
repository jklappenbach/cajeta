package dev.cajeta.idea.profiler

import com.intellij.ui.components.JBScrollPane
import com.intellij.util.ui.JBUI
import com.intellij.util.ui.UIUtil
import java.awt.BasicStroke
import java.awt.Color
import java.awt.Dimension
import java.awt.Graphics
import java.awt.Graphics2D
import java.awt.RenderingHints
import java.awt.TexturePaint
import java.awt.event.MouseAdapter
import java.awt.event.MouseEvent
import java.awt.image.BufferedImage
import javax.swing.JComponent
import javax.swing.JPanel
import javax.swing.SwingUtilities

/**
 * cajeta-profiler 11.2.c — the flame graph, painted directly (spec §8.2, §8.6).
 *
 * The surface that always works. Chosen when JCEF is unavailable, and it is a
 * complete renderer rather than a placeholder: a developer on a remote dev host
 * or a JBR without Chromium gets the same information, drawn less prettily.
 */
class SwingFlameGraphView : FlameGraphView {

    private var model: ProfileViewModel? = null
    private var track: ProfileTrackView? = null
    private var rects: List<FlameRect> = emptyList()
    private var dropped = 0
    private var select: ((FlameNode) -> Unit)? = null
    private var selectLaunch: ((FlameNode) -> Unit)? = null

    // Horizontal zoom only: a flame graph's vertical axis is stack DEPTH, a
    // count, so there is no scale there to change. Scrollable rather than a
    // bare JPanel for the same reason the timeline needed it — a bare JPanel
    // neither reports a scrollable width nor stretches to a wider viewport.
    private var zoom = HorizontalZoom.FIT

    private val canvas = object : JPanel(), javax.swing.Scrollable {
        override fun paintComponent(g: Graphics) {
            super.paintComponent(g)
            paintFlames(g as Graphics2D)
        }
        override fun getPreferredScrollableViewportSize(): Dimension = preferredSize
        override fun getScrollableUnitIncrement(
            visible: java.awt.Rectangle, orientation: Int, direction: Int
        ): Int = if (orientation == javax.swing.SwingConstants.VERTICAL) ROW_HEIGHT
                 else com.intellij.util.ui.JBUI.scale(40)
        override fun getScrollableBlockIncrement(
            visible: java.awt.Rectangle, orientation: Int, direction: Int
        ): Int = if (orientation == javax.swing.SwingConstants.VERTICAL) visible.height
                 else visible.width
        override fun getScrollableTracksViewportWidth(): Boolean {
            val vp = parent as? javax.swing.JViewport ?: return false
            return zoom <= HorizontalZoom.FIT && vp.width >= MIN_CONTENT_WIDTH
        }
        override fun getScrollableTracksViewportHeight(): Boolean = false
    }

    private val scroll = JBScrollPane(canvas)
    private var zoomChanged: ((Double) -> Unit)? = null

    override val component: JComponent get() = scroll

    override fun onSelect(handler: (FlameNode) -> Unit) { select = handler }
    override fun onSelectLaunchSite(handler: (FlameNode) -> Unit) { selectLaunch = handler }
    override fun onZoomChanged(handler: (Double) -> Unit) { zoomChanged = handler }
    override fun setZoom(zoom: Double) = applyZoom(zoom, null)

    init {
        scroll.viewport.scrollMode = javax.swing.JViewport.SIMPLE_SCROLL_MODE
        scroll.viewport.addComponentListener(object : java.awt.event.ComponentAdapter() {
            override fun componentResized(e: java.awt.event.ComponentEvent) = resizeCanvas()
        })
        // Plain wheel SCROLLS; only Ctrl zooms. Kept the same as the timeline's
        // so the two tabs do not answer the same gesture differently.
        canvas.addMouseWheelListener { e ->
            if (e.isControlDown || e.isMetaDown) {
                val factor = if (e.wheelRotation < 0) HorizontalZoom.STEP
                             else 1.0 / HorizontalZoom.STEP
                applyZoom(zoom * factor, e)
                e.consume()
            } else {
                scroll.dispatchEvent(javax.swing.SwingUtilities.convertMouseEvent(
                    canvas, e, scroll) as java.awt.event.MouseWheelEvent)
            }
        }
        canvas.background = UIUtil.getPanelBackground()
        canvas.isOpaque = true
        canvas.toolTipText = ""
        canvas.addMouseListener(object : MouseAdapter() {
            override fun mouseClicked(e: MouseEvent) {
                val hit = hitTest(e.x, e.y) ?: return
                // Right-click, or a modifier, asks for the LAUNCH site rather
                // than the frame itself. A kernel's own "source location" is the
                // kernel; §8.4 wants the line that dispatched it, and the two
                // are different places.
                if (SwingUtilities.isRightMouseButton(e) || e.isControlDown || e.isMetaDown) {
                    selectLaunch?.invoke(hit.node)
                } else {
                    select?.invoke(hit.node)
                }
            }
        })
    }

    override fun show(model: ProfileViewModel, track: ProfileTrackView) {
        this.model = model
        this.track = track
        relayout()
    }

    /**
     * Lay out for the CURRENT zoom. The threshold below which a frame is not
     * worth drawing depends on how wide the canvas is, so this has to re-run on
     * every zoom change — laying out once at load left "N frame(s) too narrow
     * to draw" saying the same number at 512x as at fit (reported 2026-09-01).
     */
    private fun relayout() {
        val m = model ?: return
        val t = track ?: return
        val (r, d) = FlameLayout.of(m, t, FlameLayout.minWidthFor(zoom))
        rects = r
        dropped = d
        resizeCanvas()
    }

    private fun resizeCanvas() {
        val vpWidth = scroll.viewport.width.takeIf { it > 0 } ?: MIN_CONTENT_WIDTH
        val depth = (rects.maxOfOrNull { it.depth } ?: 0) + 2
        canvas.preferredSize = Dimension(
            HorizontalZoom.contentWidth(vpWidth, 0, 0, MIN_CONTENT_WIDTH, zoom),
            depth * ROW_HEIGHT,
        )
        canvas.revalidate()
        canvas.repaint()
    }

    /** Zoom the time axis, keeping the span under the pointer where it is. */
    private fun applyZoom(requested: Double, at: java.awt.event.MouseWheelEvent?) {
        val next = HorizontalZoom.clampZoom(requested)
        if (next == zoom) return
        val oldWidth = canvas.width.coerceAtLeast(1)
        zoom = next
        relayout()
        // Only when the VIEW initiated it; echoing a slider-driven change back
        // would make the two chase each other.
        if (at != null) zoomChanged?.invoke(zoom)
        val vp = scroll.viewport
        val newWidth = canvas.preferredSize.width.coerceAtLeast(1)
        val maxViewX = (newWidth - vp.width).coerceAtLeast(0)
        val pointerContentX = at?.x ?: (vp.viewPosition.x + vp.width / 2)
        val pointerScreenX = if (at != null) at.x - vp.viewPosition.x else vp.width / 2
        vp.viewPosition = java.awt.Point(
            HorizontalZoom.anchoredViewX(pointerContentX, pointerScreenX,
                                         0, oldWidth, newWidth, maxViewX),
            vp.viewPosition.y)
    }

    // --- hit testing -----------------------------------------------------------

    private fun hitTest(px: Int, py: Int): FlameRect? {
        val w = canvas.width.toDouble()
        val depth = py / ROW_HEIGHT
        return rects.firstOrNull { r ->
            // Match what was PAINTED, including the one-pixel floor. A
            // zero-duration frame is drawn as a tick; hit-testing it against
            // its zero width would make it unclickable, which is the same as
            // not drawing it.
            val x0 = r.x * w
            val drawn = (r.width * w).coerceAtLeast(1.0)
            r.depth == depth && px >= x0 && px <= x0 + drawn
        }
    }

    override fun toString(): String = "SwingFlameGraphView(${rects.size} frames)"

    // --- painting ---------------------------------------------------------------

    private fun paintFlames(g: Graphics2D) {
        val m = model ?: return
        g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON)
        val w = canvas.width.toDouble()
        val fm = g.fontMetrics

        for (r in rects) {
            val quality = m.qualityOf(r.node)
            val x = (r.x * w).toInt()
            val width = (r.width * w).toInt().coerceAtLeast(1)
            val y = r.depth * ROW_HEIGHT

            g.color = FlameColors.of(quality, r.node.unclosed, r.depth)
            g.fillRect(x, y, width, ROW_HEIGHT - 1)

            // §11.3 — a flagged span is hatched as well as coloured, so the
            // distinction does not rest on hue alone.
            if (FlameColors.hatched(quality)) {
                g.paint = hatch()
                g.fillRect(x, y, width, ROW_HEIGHT - 1)
                g.paint = null
            }

            g.color = UIUtil.getPanelBackground()
            g.stroke = BasicStroke(1f)
            g.drawRect(x, y, width, ROW_HEIGHT - 1)

            // A label only where one fits. Clipping mid-word produces a name
            // that reads as a different, real method.
            if (width > MIN_LABEL_WIDTH) {
                g.color = UIUtil.getLabelForeground()
                val label = fit(r.node.name, width - 8, fm::stringWidth)
                if (label.isNotEmpty()) g.drawString(label, x + 4, y + ROW_HEIGHT - 6)
            }
        }

        if (dropped > 0) {
            g.color = UIUtil.getInactiveTextColor()
            g.drawString(
                "$dropped frame(s) too narrow to draw",
                4, (rects.maxOfOrNull { it.depth } ?: 0) * ROW_HEIGHT + ROW_HEIGHT + 12,
            )
        }
    }

    /** Truncate to fit, with an ellipsis, or return "" when nothing fits. */
    private fun fit(text: String, maxPx: Int, widthOf: (String) -> Int): String {
        if (widthOf(text) <= maxPx) return text
        var s = text
        while (s.isNotEmpty() && widthOf("$s…") > maxPx) s = s.dropLast(1)
        return if (s.isEmpty()) "" else "$s…"
    }

    private fun hatch(): TexturePaint {
        val img = BufferedImage(6, 6, BufferedImage.TYPE_INT_ARGB)
        val ig = img.createGraphics()
        ig.color = Color(0, 0, 0, 70)
        ig.drawLine(0, 6, 6, 0)
        ig.dispose()
        return TexturePaint(img, java.awt.Rectangle(0, 0, 6, 6))
    }

    private companion object {
        val ROW_HEIGHT = JBUI.scale(18)
        val MIN_LABEL_WIDTH = JBUI.scale(28)
        // Below this a flame row is a smear; the view scrolls instead.
        val MIN_CONTENT_WIDTH = JBUI.scale(600)
    }
}
