package dev.cajeta.idea.profiler

import com.intellij.ui.components.JBScrollPane
import com.intellij.util.ui.JBUI
import com.intellij.util.ui.UIUtil
import java.awt.BasicStroke
import java.awt.Dimension
import java.awt.Graphics
import java.awt.Graphics2D
import java.awt.RenderingHints
import java.awt.event.MouseAdapter
import java.awt.event.MouseEvent
import javax.swing.JComponent
import javax.swing.JPanel
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

    private val canvas = object : JPanel() {
        override fun paintComponent(g: Graphics) {
            super.paintComponent(g)
            paintTimeline(g as Graphics2D)
        }
    }

    init {
        canvas.background = UIUtil.getPanelBackground()
        canvas.isOpaque = true
        add(JBScrollPane(canvas), java.awt.BorderLayout.CENTER)
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
        canvas.preferredSize = Dimension(800, rows.size * ROW_HEIGHT + RULER_HEIGHT + 8)
        canvas.revalidate()
        canvas.repaint()
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

            g.color = UIUtil.getLabelForeground()
            val indent = row.depth * JBUI.scale(12)
            val label = "${row.view.name} (${row.view.kind.name.lowercase()})"
            g.drawString(clip(g, label, GUTTER_WIDTH - indent - 8), 4 + indent, y + ROW_HEIGHT - 6)

            g.color = UIUtil.getBoundsColor()
            g.drawLine(0, y + ROW_HEIGHT - 1, canvas.width, y + ROW_HEIGHT - 1)

            paintLane(g, m, row, y)
        }
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
        for (i in 0..TICKS) {
            val x = x0 + (w.toDouble() * i / TICKS).toInt()
            g.drawLine(x, RULER_HEIGHT - 6, x, RULER_HEIGHT - 1)
            val at = m.spanNs * i / TICKS
            g.drawString(fmt(at), x + 2, RULER_HEIGHT - 8)
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
        const val TICKS = 8
    }
}
