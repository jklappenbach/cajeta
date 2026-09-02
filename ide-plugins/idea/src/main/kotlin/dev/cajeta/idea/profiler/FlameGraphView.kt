package dev.cajeta.idea.profiler

import com.intellij.ui.JBColor
import com.intellij.ui.jcef.JBCefApp
import java.awt.Color
import javax.swing.JComponent

/**
 * cajeta-profiler 11.2.c — the flame-graph surface (spec §8.2, §8.6).
 *
 * Two implementations behind one interface, following the markdown renderer's
 * precedent exactly: [SwingFlameGraphView] always works, [JcefFlameGraphView]
 * is chosen when Chromium is available and falls back to Swing when it is not.
 * The plugin declares 242 -> 299.*, and JCEF is absent in more environments
 * than it is broken in — a remote dev host, a headless inspection run, a JBR
 * built without it. A viewer that renders nothing there would be a viewer that
 * renders nothing exactly when someone is trying to work out why a build box is
 * slow.
 */
interface FlameGraphView {

    val component: JComponent

    /** Render one track's forest against the model's shared axis. */
    fun show(model: ProfileViewModel, track: ProfileTrackView)

    /** Called when the user picks a frame — the hook §8.2 navigation hangs off. */
    fun onSelect(handler: (FlameNode) -> Unit)

    /** Called when the user asks for a kernel's launching call site (§8.4). */
    fun onSelectLaunchSite(handler: (FlameNode) -> Unit) {}

    /**
     * Set the horizontal (time-axis) zoom; 1.0 is fit-to-width. Defaulted to a
     * no-op: the JCEF renderer draws its own layout and does not share this
     * canvas's geometry, so it declines rather than pretending.
     */
    fun setZoom(zoom: Double) {}

    /** Notified when the view changes zoom ITSELF (Ctrl+scroll), so a control
     *  driving it can stay in step. */
    fun onZoomChanged(handler: (Double) -> Unit) {}
}

/**
 * The §8.6 palette.
 *
 * Colour is the whole of "visually distinct", so these are chosen to survive
 * the two things that routinely destroy such a scheme: a light/dark theme
 * switch, and a reader who cannot distinguish red from green. Each degraded
 * class differs from TRUSTED in BRIGHTNESS as well as hue, and the renderers
 * additionally hatch anything flagged, so the distinction does not rest on
 * colour alone.
 */
object FlameColors {

    /** Ordinary sampled or exactly-measured work. */
    val TRUSTED: JBColor = JBColor(Color(0xE8, 0x8B, 0x3C), Color(0xC9, 0x77, 0x33))

    /** A measurement one rung down §10.4's ladder. */
    val DEGRADED: JBColor = JBColor(Color(0xB0, 0x8A, 0x6A), Color(0x8A, 0x6E, 0x55))

    /** The clock fit behind it is weak. */
    val LOW_CONFIDENCE: JBColor = JBColor(Color(0xC4, 0xA5, 0x6B), Color(0x9C, 0x84, 0x56))

    /** §11.6 — no trustworthy correlation. Deliberately drab. */
    val UNCORRELATED: JBColor = JBColor(Color(0x9A, 0x9A, 0x9A), Color(0x6E, 0x6E, 0x6E))

    /** §11.3 — the integrity checker raised something. */
    val FLAGGED: JBColor = JBColor(Color(0xC4, 0x5B, 0x5B), Color(0xA5, 0x4B, 0x4B))

    /** A frame still running when the trace ended. */
    val UNCLOSED: JBColor = JBColor(Color(0x7A, 0x9E, 0xC4), Color(0x5B, 0x7C, 0xA5))

    /**
     * One hue per stack layer, for TRUSTED work only.
     *
     * Colour on this graph was already spoken for — §8.6 needs degraded and
     * low-confidence measurements visually distinct, and §11.3's flagged spans
     * must not blend in. Letting depth own the hue everywhere would have erased
     * that: FLAGGED is hatched as well and would survive, but DEGRADED,
     * LOW_CONFIDENCE and UNCORRELATED have no encoding but colour.
     *
     * So the two live on separate axes. Depth picks the hue where the profiler
     * trusts the measurement; anything it does not keeps its own colour, which
     * makes it MORE conspicuous than before — it breaks the rainbow instead of
     * being a slightly different brown.
     *
     * Hues are evenly spaced around the wheel and deliberately kept off the
     * reds and greys the quality colours occupy.
     */
    const val RAINBOW_PERIOD = 12

    private val RAINBOW: List<JBColor> = (0 until RAINBOW_PERIOD).map { i ->
        val hue = i.toFloat() / RAINBOW_PERIOD
        JBColor(
            Color(Color.HSBtoRGB(hue, 0.62f, 0.86f)),   // light theme
            Color(Color.HSBtoRGB(hue, 0.55f, 0.68f)),   // dark theme
        )
    }

    /** The layer hue for [depth]; repeats rather than running out, so a deep
     *  stack stays coloured instead of flattening to one shade. */
    fun layer(depth: Int): JBColor =
        RAINBOW[((depth % RAINBOW_PERIOD) + RAINBOW_PERIOD) % RAINBOW_PERIOD]

    fun of(quality: MeasurementQuality?, unclosed: Boolean = false,
           depth: Int = 0): JBColor = when {
        unclosed -> UNCLOSED
        quality == null -> layer(depth)
        else -> when (quality.renderClass) {
            RenderClass.TRUSTED -> layer(depth)
            RenderClass.LOW_CONFIDENCE -> LOW_CONFIDENCE
            RenderClass.DEGRADED -> DEGRADED
            RenderClass.UNCORRELATED -> UNCORRELATED
            RenderClass.FLAGGED -> FLAGGED
        }
    }

    /**
     * Whether the frame is drawn with a hatch over its fill. Colour alone would
     * make the flagged/normal distinction invisible to a reader with a common
     * colour vision deficiency, and this is the one distinction §11.3 says must
     * not be missed.
     */
    fun hatched(quality: MeasurementQuality?): Boolean = quality?.flagged == true

    fun cssOf(quality: MeasurementQuality?, unclosed: Boolean = false,
              depth: Int = 0): String {
        val c = of(quality, unclosed, depth)
        return "#%02x%02x%02x".format(c.red, c.green, c.blue)
    }
}

object FlameGraphViewFactory {

    fun jcefAvailable(): Boolean = runCatching { JBCefApp.isSupported() }.getOrDefault(false)

    /**
     * JCEF when it is there, Swing when it is not.
     *
     * Unlike the markdown surface there is no settings opt-in: a flame graph is
     * a whole tool window rather than an experimental fold decoration, and the
     * Swing surface here is a complete renderer rather than a placeholder.
     */
    fun create(): FlameGraphView =
        if (jcefAvailable()) runCatching { JcefFlameGraphView() as FlameGraphView }
            .getOrElse { SwingFlameGraphView() }
        else SwingFlameGraphView()
}

/**
 * Frame geometry, shared by both surfaces so they lay out identically.
 *
 * Kept out of the renderers because it is the part worth testing: a rectangle
 * is checkable, a painted pixel is not.
 */
data class FlameRect(
    val node: FlameNode,
    val depth: Int,
    /** 0.0 .. 1.0 across the model's shared axis. */
    val x: Double,
    val width: Double,
)

object FlameLayout {

    /**
     * Lay out a track's forest against the model's axis.
     *
     * Placed on the SHARED axis, not on the track's own extent (§8.3). A device
     * queue busy for 3% of the run must be drawn as 3% of the width; scaling it
     * to its own extent draws it as a full-width bar and tells the reader the
     * opposite of what happened.
     *
     * Frames narrower than [minWidth] of the axis are dropped along with their
     * subtrees — at one pixel they are not readable, and keeping them makes a
     * deep tree cost time proportional to samples rather than to what is
     * visible. The count of what was dropped is returned so the UI can say so
     * rather than quietly showing less than the trace holds.
     */
    /** Narrowest frame worth drawing at fit, as a fraction of the axis. */
    const val MIN_WIDTH = 0.0005

    /**
     * The threshold at [zoom]. Zooming exists to make narrow frames readable,
     * so the bar has to come down as the canvas gets wider — a fixed fraction
     * means the same frames stay hidden however far you zoom in, which is the
     * one thing zoom was supposed to fix.
     */
    fun minWidthFor(zoom: Double): Double =
        MIN_WIDTH / HorizontalZoom.clampZoom(zoom)

    fun of(
        model: ProfileViewModel,
        track: ProfileTrackView,
        minWidth: Double = MIN_WIDTH,
    ): Pair<List<FlameRect>, Int> {
        val out = ArrayList<FlameRect>()
        var dropped = 0

        fun place(node: FlameNode, depth: Int) {
            val x = model.fractionOf(node.startNs)
            val w = if (model.spanNs <= 0) 0.0
            else (node.inclusiveNs.toDouble() / model.spanNs.toDouble()).coerceIn(0.0, 1.0 - x)
            // A ZERO-duration frame is not narrow, it is an OBSERVATION: the
            // frame was on the stack for exactly one sample tick. No zoom can
            // widen zero, so a width test hides it at every zoom — which is
            // how "2 frame(s) too narrow to draw" came to report the same
            // count at 512x as at fit (2026-09-01), naming frames that could
            // never be reached. It is drawn at the renderer's one-pixel floor
            // instead, for the reason the timeline already gives: rounding a
            // real event away draws an idle lane that was not idle.
            if (w > 0.0 && w < minWidth) {
                dropped += 1 + countDescendants(node)
                return
            }
            out.add(FlameRect(node, depth, x, w))
            node.children.forEach { place(it, depth + 1) }
        }
        track.roots.forEach { place(it, 0) }
        return out to dropped
    }

    private fun countDescendants(n: FlameNode): Int =
        n.children.sumOf { 1 + countDescendants(it) }
}
