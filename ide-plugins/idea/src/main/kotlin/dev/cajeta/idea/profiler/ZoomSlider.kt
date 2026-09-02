package dev.cajeta.idea.profiler

import com.intellij.ui.components.JBLabel
import com.intellij.util.ui.JBUI
import java.awt.Dimension
import java.awt.FlowLayout
import javax.swing.JPanel
import javax.swing.JSlider

/**
 * The time-axis zoom control, shared by the flame graph and the timeline.
 *
 * Right-justified on the view's own toolbar row rather than given a row of its
 * own: it is a property of the view being looked at, and a second bar would
 * cost a line of vertical space in a tool window that has little to spare.
 *
 * The slider is linear in LOG space (see [HorizontalZoom.sliderToZoom]) because
 * zoom is geometric — a linear slider would spend most of its travel above
 * 400x and leave the useful low range unusable.
 */
class ZoomSlider(private val onZoom: (Double) -> Unit) :
    JPanel(FlowLayout(FlowLayout.RIGHT, JBUI.scale(4), 0)) {

    private val slider = JSlider(0, HorizontalZoom.SLIDER_STEPS, 0)
    private val readout = JBLabel(HorizontalZoom.label(HorizontalZoom.FIT))
    /** Set while reflecting an external change, so echoing it back is not
     *  mistaken for the user dragging. */
    private var reflecting = false

    init {
        slider.preferredSize = Dimension(JBUI.scale(120), slider.preferredSize.height)
        slider.toolTipText = "Zoom the time axis (Ctrl+scroll over the graph)"
        readout.preferredSize = Dimension(JBUI.scale(40), readout.preferredSize.height)
        add(JBLabel("Zoom:"))
        add(slider)
        add(readout)
        slider.addChangeListener {
            if (reflecting) return@addChangeListener
            val zoom = HorizontalZoom.sliderToZoom(slider.value)
            readout.text = HorizontalZoom.label(zoom)
            onZoom(zoom)
        }
    }

    /** Reflect a zoom the VIEW chose (Ctrl+scroll), without firing back. */
    fun reflect(zoom: Double) {
        reflecting = true
        slider.value = HorizontalZoom.zoomToSlider(zoom)
        readout.text = HorizontalZoom.label(zoom)
        reflecting = false
    }
}
