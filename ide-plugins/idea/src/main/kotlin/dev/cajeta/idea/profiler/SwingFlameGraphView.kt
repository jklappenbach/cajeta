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

    private val canvas = object : JPanel() {
        override fun paintComponent(g: Graphics) {
            super.paintComponent(g)
            paintFlames(g as Graphics2D)
        }
    }

    private val scroll = JBScrollPane(canvas)

    override val component: JComponent get() = scroll

    override fun onSelect(handler: (FlameNode) -> Unit) { select = handler }
    override fun onSelectLaunchSite(handler: (FlameNode) -> Unit) { selectLaunch = handler }

    init {
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
        val (r, d) = FlameLayout.of(model, track)
        rects = r
        dropped = d
        canvas.preferredSize = Dimension(
            canvas.width.coerceAtLeast(600),
            ((r.maxOfOrNull { it.depth } ?: 0) + 2) * ROW_HEIGHT,
        )
        canvas.revalidate()
        canvas.repaint()
    }

    // --- hit testing -----------------------------------------------------------

    private fun hitTest(px: Int, py: Int): FlameRect? {
        val w = canvas.width.toDouble()
        val depth = py / ROW_HEIGHT
        return rects.firstOrNull { r ->
            r.depth == depth && px >= r.x * w && px <= (r.x + r.width) * w
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

            g.color = FlameColors.of(quality, r.node.unclosed)
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
    }
}
