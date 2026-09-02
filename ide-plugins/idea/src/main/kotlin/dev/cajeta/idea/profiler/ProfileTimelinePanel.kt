package dev.cajeta.idea.profiler

import com.intellij.ui.components.JBLabel
import com.intellij.ui.components.JBScrollPane
import com.intellij.util.ui.JBUI
import com.intellij.util.ui.UIUtil
import java.awt.BasicStroke
import java.awt.Dimension
import java.awt.Graphics
import java.awt.Graphics2D
import java.awt.Rectangle
import java.awt.RenderingHints
import java.awt.FlowLayout
import java.awt.event.MouseAdapter
import java.awt.event.MouseEvent
import java.awt.event.MouseWheelEvent
import javax.swing.JButton
import javax.swing.JComponent
import javax.swing.JPanel
import javax.swing.JViewport
import javax.swing.ScrollPaneConstants
import javax.swing.Scrollable
import javax.swing.SwingConstants
import javax.swing.SwingUtilities

/**
 * cajeta-profiler 11.2.d — host threads, fibers and device queues on one axis
 * (spec §8.3).
 *
 * The flame graph answers "what did this lane spend its time in". The timeline
 * answers the question that only exists once there is more than one lane: what
 * was everything else doing at that moment. That is why every track is drawn
 * against the model's SHARED axis — a lane scaled to its own extent is a lane
 * that cannot be compared to any other, which is the entire purpose of the view.
 *
 * Hierarchy is drawn as indentation from [TrackHierarchy], so four queues under
 * one device read as one device with four lanes rather than four devices.
 */
class ProfileTimelinePanel : JPanel(java.awt.BorderLayout()) {

    private var model: ProfileViewModel? = null
    private var rows: List<TrackNode> = emptyList()
    private var select: ((FlameNode) -> Unit)? = null
    // Horizontal zoom only. The vertical axis is a LIST of tracks, not a scale,
    // so there is nothing there to magnify.
    private var zoom = HorizontalZoom.FIT

    // Scrollable, not a bare JPanel: a bare JPanel neither reports a height the
    // viewport can scroll nor stretches to a viewport wider than itself, which
    // is why the timeline had no scrollbars and no use for extra width.
    private val canvas = object : JPanel(), Scrollable {
        override fun paintComponent(g: Graphics) {
            super.paintComponent(g)
            paintTimeline(g as Graphics2D)
        }

        override fun getPreferredScrollableViewportSize(): Dimension = preferredSize

        // A row at a time vertically — the unit the reader actually thinks in.
        override fun getScrollableUnitIncrement(
            visible: Rectangle, orientation: Int, direction: Int
        ): Int = if (orientation == SwingConstants.VERTICAL) ROW_HEIGHT else JBUI.scale(40)

        override fun getScrollableBlockIncrement(
            visible: Rectangle, orientation: Int, direction: Int
        ): Int = if (orientation == SwingConstants.VERTICAL) visible.height else visible.width

        // Stretch to the viewport only at FIT. Once zoomed in the canvas is
        // deliberately wider than the viewport — that width is what there is to
        // scroll, and reporting "I track the viewport" would throw it away.
        override fun getScrollableTracksViewportWidth(): Boolean {
            val vp = parent as? JViewport ?: return false
            return TimelineViewport.tracksViewportWidth(vp.width, MIN_CONTENT_WIDTH, zoom)
        }

        override fun getScrollableTracksViewportHeight(): Boolean {
            val vp = parent as? JViewport ?: return false
            return TimelineViewport.tracksViewportHeight(vp.height, preferredSize.height)
        }
    }

    private val scroll = JBScrollPane(canvas)
    // Right-justified on its own row: the timeline has no track selector to
    // share one with, but it reads the same as the flame graph's.
    private val zoomSlider = ZoomSlider { z -> applyZoom(z, null) }

    init {
        canvas.background = UIUtil.getPanelBackground()
        canvas.isOpaque = true
        // Stated rather than inherited: these are the behaviour under test.
        scroll.verticalScrollBarPolicy = ScrollPaneConstants.VERTICAL_SCROLLBAR_AS_NEEDED
        scroll.horizontalScrollBarPolicy = ScrollPaneConstants.HORIZONTAL_SCROLLBAR_AS_NEEDED
        // Below this the lanes stop being readable, so the view scrolls instead
        // of compressing further.
        canvas.minimumSize = Dimension(MIN_CONTENT_WIDTH, 0)
        canvas.preferredSize = Dimension(MIN_CONTENT_WIDTH, RULER_HEIGHT + 8)
        // Left-justified, so it sits where the flame graph's does — the two
        // tabs are the same run looked at two ways and should not move their
        // controls around.
        add(JPanel(java.awt.BorderLayout()).also { it.add(zoomSlider, java.awt.BorderLayout.WEST) },
            java.awt.BorderLayout.NORTH)
        add(scroll, java.awt.BorderLayout.CENTER)
        // The track-name gutter is painted at the current view origin so it
        // stays put while the lanes scroll under it. That needs a full repaint
        // per scroll: the default blit mode moves pixels and would smear the
        // pinned band across the lanes.
        scroll.viewport.scrollMode = JViewport.SIMPLE_SCROLL_MODE
        scroll.viewport.addChangeListener { canvas.repaint() }
        // Zoom is expressed against the viewport width, so a resize rescales.
        scroll.viewport.addComponentListener(object : java.awt.event.ComponentAdapter() {
            override fun componentResized(e: java.awt.event.ComponentEvent) = resizeCanvas()
        })
        // Plain wheel SCROLLS; only Ctrl zooms. The wheel can mean one thing,
        // and with a long track list vertical scrolling is what a reader needs
        // from it — a bare-wheel zoom was tried and reverted on 2026-09-01 for
        // exactly that reason.
        canvas.addMouseWheelListener { e ->
            if (e.isControlDown || e.isMetaDown) {
                val factor = if (e.wheelRotation < 0) HorizontalZoom.STEP
                             else 1.0 / HorizontalZoom.STEP
                applyZoom(zoom * factor, e)
                e.consume()
            } else {
                scroll.dispatchEvent(javax.swing.SwingUtilities.convertMouseEvent(
                    canvas, e, scroll) as MouseWheelEvent)
            }
        }
        canvas.addMouseListener(object : MouseAdapter() {
            override fun mouseClicked(e: MouseEvent) {
                if (!SwingUtilities.isLeftMouseButton(e)) return
                hitTest(e.x, e.y)?.let { select?.invoke(it) }
            }
        })
    }

    fun onSelect(handler: (FlameNode) -> Unit) { select = handler }

    fun show(model: ProfileViewModel) {
        this.model = model
        // Structure-only tracks (a device, a context) are kept as ROWS because
        // they carry the hierarchy, even though they hold no slices of their
        // own. Dropping them would leave queues indented under nothing.
        rows = TrackHierarchy.flatten(model.tracks)
        resizeCanvas()
    }

    /** Canvas size for the current zoom and track count. */
    private fun resizeCanvas() {
        val vpWidth = scroll.viewport.width.takeIf { it > 0 } ?: MIN_CONTENT_WIDTH
        canvas.preferredSize = Dimension(
            HorizontalZoom.contentWidth(vpWidth, GUTTER_WIDTH, 8, MIN_CONTENT_WIDTH, zoom),
            TimelineViewport.contentHeight(rows.size, ROW_HEIGHT, RULER_HEIGHT, 8))
        canvas.revalidate()
        canvas.repaint()
    }

    /**
     * Set the zoom, keeping the instant under the pointer where it is. Without
     * the anchor, zooming walks the region of interest off-screen and the
     * reader has to chase it back with the scrollbar.
     */
    private fun applyZoom(requested: Double, at: MouseWheelEvent?) {
        val next = HorizontalZoom.clampZoom(requested)
        if (next == zoom) return
        val oldLane = laneWidth()
        zoom = next
        resizeCanvas()
        // Only a wheel-initiated change echoes to the slider; a slider-driven
        // one must not be sent back, or the two chase each other.
        if (at != null) zoomSlider.reflect(zoom)
        val vp = scroll.viewport
        val newLane = (canvas.preferredSize.width - GUTTER_WIDTH - 8).coerceAtLeast(1)
        val maxViewX = (canvas.preferredSize.width - vp.width).coerceAtLeast(0)
        val pointerContentX = at?.x ?: (vp.viewPosition.x + vp.width / 2)
        val pointerScreenX = if (at != null) at.x - vp.viewPosition.x else vp.width / 2
        vp.viewPosition = java.awt.Point(
            HorizontalZoom.anchoredViewX(pointerContentX, pointerScreenX,
                                         GUTTER_WIDTH, oldLane, newLane, maxViewX),
            vp.viewPosition.y)
    }

    // --- geometry ---------------------------------------------------------------

    private fun laneX(): Int = GUTTER_WIDTH

    private fun laneWidth(): Int = (canvas.width - GUTTER_WIDTH - 8).coerceAtLeast(1)

    private fun hitTest(px: Int, py: Int): FlameNode? {
        val m = model ?: return null
        val row = (py - RULER_HEIGHT) / ROW_HEIGHT
        val node = rows.getOrNull(row) ?: return null
        if (px < laneX()) return null
        val f = (px - laneX()).toDouble() / laneWidth().toDouble()
        val ns = m.startNs + (f * m.spanNs).toLong()
        // Deepest frame covering that instant: the innermost is what the reader
        // pointed at, and it is the one worth navigating to.
        return deepestAt(node.view.roots, ns)
    }

    private fun deepestAt(nodes: List<FlameNode>, ns: Long): FlameNode? {
        for (n in nodes) {
            if (ns < n.startNs || ns > n.startNs + n.inclusiveNs) continue
            return deepestAt(n.children, ns) ?: n
        }
        return null
    }

    // --- painting ----------------------------------------------------------------

    private fun paintTimeline(g: Graphics2D) {
        val m = model ?: return
        g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON)
        paintRuler(g, m)

        for ((i, row) in rows.withIndex()) {
            val y = RULER_HEIGHT + i * ROW_HEIGHT
            g.color = UIUtil.getBoundsColor()
            g.drawLine(0, y + ROW_HEIGHT - 1, canvas.width, y + ROW_HEIGHT - 1)
            paintLane(g, m, row, y)
        }

        // The gutter holds track NAMES, not time, so it does not scroll with
        // the lanes — it is repainted at the current view origin, over whatever
        // lane pixels ran under it. A gutter that scrolled away would leave a
        // zoomed timeline as anonymous bars.
        val viewX = (canvas.parent as? JViewport)?.viewPosition?.x ?: 0
        g.color = UIUtil.getPanelBackground()
        g.fillRect(viewX, 0, GUTTER_WIDTH, canvas.height)
        for ((i, row) in rows.withIndex()) {
            val y = RULER_HEIGHT + i * ROW_HEIGHT
            g.color = UIUtil.getLabelForeground()
            val indent = row.depth * JBUI.scale(12)
            val label = "${row.view.name} (${row.view.kind.name.lowercase()})"
            g.drawString(clip(g, label, GUTTER_WIDTH - indent - 8),
                         viewX + 4 + indent, y + ROW_HEIGHT - 6)
        }
        g.color = UIUtil.getBoundsColor()
        g.drawLine(viewX + GUTTER_WIDTH - 1, 0, viewX + GUTTER_WIDTH - 1, canvas.height)
    }

    /**
     * One track's occupancy.
     *
     * Only ROOT frames are drawn. A timeline is about when a lane was busy, not
     * about what it was busy in — the flame graph answers that, and drawing
     * every nested frame here produces a solid bar that says nothing.
     */
    private fun paintLane(g: Graphics2D, m: ProfileViewModel, row: TrackNode, y: Int) {
        val x0 = laneX()
        val w = laneWidth()
        for (n in row.view.roots) {
            val fx = m.fractionOf(n.startNs)
            val fw = if (m.spanNs <= 0) 0.0
            else (n.inclusiveNs.toDouble() / m.spanNs.toDouble())
            val px = x0 + (fx * w).toInt()
            // At least one pixel: a kernel that ran for 8 us inside a 30 s run
            // is 0.00003 of the width, and rounding it away draws an idle device
            // that was not idle.
            val pw = (fw * w).toInt().coerceAtLeast(1)

            g.color = FlameColors.of(m.qualityOf(n), n.unclosed)
            g.fillRect(px, y + 3, pw, ROW_HEIGHT - 8)
            if (FlameColors.hatched(m.qualityOf(n))) {
                g.color = java.awt.Color(0, 0, 0, 60)
                g.fillRect(px, y + 3, pw, ROW_HEIGHT - 8)
            }
        }
    }

    private fun paintRuler(g: Graphics2D, m: ProfileViewModel) {
        g.color = UIUtil.getInactiveTextColor()
        g.stroke = BasicStroke(1f)
        val x0 = laneX()
        val w = laneWidth()
        val right = x0 + w
        for (i in 0..TICKS) {
            val x = x0 + (w.toDouble() * i / TICKS).toInt()
            g.drawLine(x, RULER_HEIGHT - 6, x, RULER_HEIGHT - 1)
            val at = m.spanNs * i / TICKS
            val text = fmt(at)
            // The LAST tick sits on the right edge, so a label drawn to its
            // right is off the canvas: the run's own total — the one number a
            // reader most wants — was the only one never shown. Flip any label
            // that would overflow to the left of its tick.
            val textW = g.fontMetrics.stringWidth(text)
            val tx = if (x + 2 + textW > right) x - 2 - textW else x + 2
            g.drawString(text, tx.coerceAtLeast(x0), RULER_HEIGHT - 8)
        }
        g.drawLine(x0, RULER_HEIGHT - 1, x0 + w, RULER_HEIGHT - 1)
    }

    private fun clip(g: Graphics2D, s: String, maxPx: Int): String {
        val fm = g.fontMetrics
        if (fm.stringWidth(s) <= maxPx) return s
        var t = s
        while (t.isNotEmpty() && fm.stringWidth("$t…") > maxPx) t = t.dropLast(1)
        return "$t…"
    }

    private fun fmt(ns: Long): String = when {
        ns >= 1_000_000_000 -> "%.1fs".format(ns / 1e9)
        ns >= 1_000_000 -> "%.1fms".format(ns / 1e6)
        ns >= 1_000 -> "%.1fus".format(ns / 1e3)
        else -> "${ns}ns"
    }

    val component: JComponent get() = this

    private companion object {
        val ROW_HEIGHT = JBUI.scale(22)
        val RULER_HEIGHT = JBUI.scale(18)
        val GUTTER_WIDTH = JBUI.scale(190)
        // Gutter plus enough lane to tell one span from another.
        val MIN_CONTENT_WIDTH = GUTTER_WIDTH + JBUI.scale(320)
        const val TICKS = 8
    }
}
