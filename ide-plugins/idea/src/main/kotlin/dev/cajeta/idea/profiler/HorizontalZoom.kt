package dev.cajeta.idea.profiler

/**
 * Zoom along the TIME axis, shared by the timeline and the flame graph.
 *
 * Both views map a whole run onto the available width, so at fit there is
 * nothing to scroll horizontally and a 30 s run renders an 8 us span as one
 * pixel. Zoom stretches time only — neither view has a vertical scale to
 * change: the timeline's rows are a list of tracks and the flame graph's are
 * stack depth.
 *
 * Pure arithmetic, so the behaviour is testable without a viewport.
 */
object HorizontalZoom {

    // At fit, the whole run is mapped onto the available width, so there is
    // nothing to scroll horizontally and a 30 s run renders an 8 us kernel as
    // one pixel. Zoom stretches the TIME axis only — rows keep their height,
    // because the vertical axis is a list of tracks and has no scale to change.

    const val FIT = 1.0
    const val MAX = 512.0
    /** One notch. Geometric, so zooming out undoes zooming in exactly. */
    const val STEP = 1.5

    fun clampZoom(zoom: Double): Double = when {
        zoom.isNaN() -> FIT
        zoom < FIT -> FIT        // never narrower than the viewport
        zoom > MAX -> MAX
        else -> zoom
    }

    /**
     * Total canvas width at [zoom]. The gutter is FIXED — it holds track names,
     * not time — so only the lane area scales, and zooming does not push the
     * names off by their own width.
     */
    fun contentWidth(viewportWidth: Int, gutterWidth: Int, pad: Int,
                     minContentWidth: Int, zoom: Double): Int {
        val laneFit = (viewportWidth - gutterWidth - pad).coerceAtLeast(1)
        val lane = (laneFit * clampZoom(zoom)).toInt().coerceAtLeast(1)
        return (gutterWidth + pad + lane).coerceAtLeast(minContentWidth)
    }

    /**
     * Where to put the horizontal view origin so the instant under the pointer
     * stays under the pointer across a zoom. Without this, zooming walks the
     * region of interest off-screen and the reader has to chase it.
     *
     * [pointerContentX] and the result are content coordinates; [pointerScreenX]
     * is the pointer's offset within the viewport.
     */
    fun anchoredViewX(pointerContentX: Int, pointerScreenX: Int,
                      gutterWidth: Int, oldLaneWidth: Int, newLaneWidth: Int,
                      maxViewX: Int): Int {
        if (oldLaneWidth <= 0) return 0
        val fraction = (pointerContentX - gutterWidth).toDouble() / oldLaneWidth
        val newContentX = gutterWidth + (fraction * newLaneWidth).toInt()
        val viewX = newContentX - pointerScreenX
        return viewX.coerceIn(0, maxViewX.coerceAtLeast(0))
    }

    /**
     * Fill a viewport taller than the content, scroll when it is shorter.
     * Without the first half a short track list leaves the viewport's own
     * background showing under the rows, which reads as a rendering fault.
     */
    fun tracksViewportHeight(viewportHeight: Int, contentHeight: Int): Boolean =
        viewportHeight >= contentHeight

    // --- slider mapping ------------------------------------------------------
    //
    // Zoom is GEOMETRIC — each notch multiplies — so a linear slider over the
    // zoom value would spend most of its travel between 400x and 512x and give
    // the useful 1x-8x range a few pixels. The slider is linear in log space,
    // which makes equal travel mean equal ratio.

    /** Slider notches. Arbitrary resolution, not a zoom bound. */
    const val SLIDER_STEPS = 100

    fun sliderToZoom(value: Int, steps: Int = SLIDER_STEPS): Double {
        if (steps <= 0) return FIT
        val t = value.coerceIn(0, steps).toDouble() / steps
        return clampZoom(Math.pow(MAX / FIT, t) * FIT)
    }

    fun zoomToSlider(zoom: Double, steps: Int = SLIDER_STEPS): Int {
        if (steps <= 0) return 0
        val z = clampZoom(zoom)
        val t = Math.log(z / FIT) / Math.log(MAX / FIT)
        return Math.round(t * steps).toInt().coerceIn(0, steps)
    }

    /** "100%", "250%", "12x" — percent reads wrong once it is large. */
    fun label(zoom: Double): String {
        val z = clampZoom(zoom)
        return if (z < 10.0) "${Math.round(z * 100).toInt()}%" else "${Math.round(z).toInt()}x"
    }
}
