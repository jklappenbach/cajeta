package dev.cajeta.idea.profiler

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Zoom along the time axis, shared by the timeline and the flame graph.
 *
 * Both views map a whole run onto the available width, so at fit there is
 * nothing to scroll horizontally — a 70 ms tour run puts every microsecond-
 * scale span inside a couple of pixels. Zoom is what creates width to scroll.
 */
class HorizontalZoomTest {

    @Test
    fun zoomNeverGoesBelowFit() {
        // Narrower than the viewport would mean empty space beside the content
        // rather than a smaller picture, which is not a thing the view can show.
        assertEquals(HorizontalZoom.FIT, HorizontalZoom.clampZoom(0.5), 1e-9)
        assertEquals(HorizontalZoom.FIT, HorizontalZoom.clampZoom(-3.0), 1e-9)
        assertEquals(HorizontalZoom.FIT, HorizontalZoom.clampZoom(Double.NaN), 1e-9)
    }

    @Test
    fun zoomIsCapped() {
        assertEquals(HorizontalZoom.MAX, HorizontalZoom.clampZoom(1e9), 1e-9)
    }

    @Test
    fun stepsAreReversible() {
        val once = HorizontalZoom.clampZoom(HorizontalZoom.FIT * HorizontalZoom.STEP)
        val back = HorizontalZoom.clampZoom(once / HorizontalZoom.STEP)
        assertEquals(HorizontalZoom.FIT, back, 1e-9)
    }

    @Test
    fun atFitTheContentIsTheViewport() {
        // 1000 wide, 190 gutter, 8 pad -> 802 of lane, back to 1000 total.
        assertEquals(1000,
            HorizontalZoom.contentWidth(1000, 190, 8, 500, HorizontalZoom.FIT))
    }

    @Test
    fun zoomingScalesOnlyTheLaneNotTheGutter() {
        // The gutter holds track names, not time; doubling the zoom must not
        // double it, or the names march off by their own width.
        val fit = HorizontalZoom.contentWidth(1000, 190, 8, 500, 1.0)
        val two = HorizontalZoom.contentWidth(1000, 190, 8, 500, 2.0)
        assertEquals(1000, fit)
        assertEquals(190 + 8 + 802 * 2, two)
    }

    @Test
    fun aZoomedCanvasIsWiderThanItsViewport() {
        // The whole point: something to scroll.
        assertTrue(HorizontalZoom.contentWidth(1000, 190, 8, 500, 4.0) > 1000)
    }

    @Test
    fun theMinimumStillApplies() {
        assertEquals(500, HorizontalZoom.contentWidth(200, 190, 8, 500, HorizontalZoom.FIT))
    }

    @Test
    fun theInstantUnderThePointerStaysUnderThePointer() {
        // Pointer at content x=500 in a 1000-wide lane (halfway), sitting 300 px
        // into the viewport. After a 2x zoom that instant is at 1000, so the
        // view origin must be 1000-300 = 700 for it not to move on screen.
        assertEquals(700,
            HorizontalZoom.anchoredViewX(500, 300, 0, 1000, 2000, 4000))
    }

    @Test
    fun theAnchorRespectsTheScrollableRange() {
        // Never past the end, never before the start.
        assertEquals(0, HorizontalZoom.anchoredViewX(10, 300, 0, 1000, 2000, 4000))
        assertEquals(50, HorizontalZoom.anchoredViewX(990, 0, 0, 1000, 2000, 50))
    }

    @Test
    fun aDegenerateOldWidthDoesNotDivideByZero() {
        assertEquals(0, HorizontalZoom.anchoredViewX(500, 300, 0, 0, 2000, 4000))
    }

    // --- slider mapping ------------------------------------------------------

    @Test
    fun theSliderEndsAreFitAndMax() {
        assertEquals(HorizontalZoom.FIT, HorizontalZoom.sliderToZoom(0), 1e-9)
        assertEquals(HorizontalZoom.MAX,
            HorizontalZoom.sliderToZoom(HorizontalZoom.SLIDER_STEPS), 1e-6)
    }

    @Test
    fun theSliderRoundTrips() {
        for (v in listOf(0, 7, 25, 50, 75, 99, HorizontalZoom.SLIDER_STEPS)) {
            val z = HorizontalZoom.sliderToZoom(v)
            assertEquals("notch $v", v, HorizontalZoom.zoomToSlider(z))
        }
    }

    @Test
    fun theSliderIsLogarithmicNotLinear() {
        // Equal travel must mean equal RATIO. A linear slider would put the
        // midpoint at ~256x and leave the useful 1x-8x range in a few pixels.
        val mid = HorizontalZoom.sliderToZoom(HorizontalZoom.SLIDER_STEPS / 2)
        assertEquals(Math.sqrt(HorizontalZoom.MAX), mid, 1e-6)
        assertTrue("midpoint $mid must be far below linear's 256x", mid < 64.0)

        val quarter = HorizontalZoom.sliderToZoom(HorizontalZoom.SLIDER_STEPS / 4)
        val threeQ = HorizontalZoom.sliderToZoom(3 * HorizontalZoom.SLIDER_STEPS / 4)
        assertEquals("equal travel, equal ratio",
            mid / quarter, threeQ / mid, 1e-6)
    }

    @Test
    fun outOfRangeNotchesAreClamped() {
        assertEquals(HorizontalZoom.FIT, HorizontalZoom.sliderToZoom(-5), 1e-9)
        assertEquals(HorizontalZoom.MAX, HorizontalZoom.sliderToZoom(10_000), 1e-6)
    }

    @Test
    fun theReadoutSwitchesFromPercentToMultiplier() {
        // "51200%" is unreadable; past 10x a multiplier is what people say.
        assertEquals("100%", HorizontalZoom.label(1.0))
        assertEquals("250%", HorizontalZoom.label(2.5))
        assertEquals("12x", HorizontalZoom.label(12.0))
        assertEquals("512x", HorizontalZoom.label(HorizontalZoom.MAX))
    }
}
