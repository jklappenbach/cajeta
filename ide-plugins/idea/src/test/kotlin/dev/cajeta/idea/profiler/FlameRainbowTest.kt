package dev.cajeta.idea.profiler

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Layer colouring (Julian, 2026-08-31: "Make the colors of each layer on the
 * flame graph a different color. Rainbow.").
 *
 * Colour on this graph was ALREADY carrying meaning — measurement quality, per
 * spec §8.6/§11.3 and plan 11.1.f. A straight depth-rainbow would have erased
 * it: `FLAGGED` is also hatched and would survive, but `DEGRADED`,
 * `LOW_CONFIDENCE` and `UNCORRELATED` have no encoding but colour and would
 * have become invisible.
 *
 * So depth owns the hue only where the measurement is trusted. Anything the
 * profiler does not fully trust keeps its distinct colour, which makes it MORE
 * conspicuous than before: it now breaks a rainbow instead of being a slightly
 * different brown.
 */
class FlameRainbowTest {

    /** A quality that lands in the wanted render class. */
    private fun quality(cls: RenderClass): MeasurementQuality? = when (cls) {
        RenderClass.TRUSTED -> null      // the ordinary sampled frame
        RenderClass.FLAGGED ->
            MeasurementQuality(ProfileTier.DEVICE, 100, SpanIntegrity.NEGATIVE)
        RenderClass.UNCORRELATED ->
            MeasurementQuality(ProfileTier.DEVICE, 0, SpanIntegrity.OK)
        RenderClass.DEGRADED ->
            MeasurementQuality(ProfileTier.HOST, 100, SpanIntegrity.OK)
        RenderClass.LOW_CONFIDENCE ->
            MeasurementQuality(ProfileTier.DEVICE, 20, SpanIntegrity.OK)
    }

    @Test
    fun consecutiveLayersOfTrustedWorkDiffer() {
        val seen = (0 until 6).map { FlameColors.of(null, false, it).rgb }
        assertEquals("each layer is its own hue", seen.size, seen.distinct().size)
    }

    @Test
    fun theRainbowRepeatsRatherThanRunningOut() {
        // A deep stack must stay coloured; running out of hues and falling back
        // to one flat colour would silently undo the feature at depth.
        val deep = FlameColors.of(null, false, 200)
        assertTrue(deep.rgb != 0)
        assertEquals(FlameColors.of(null, false, 0).rgb,
                     FlameColors.of(null, false, FlameColors.RAINBOW_PERIOD).rgb)
    }

    @Test
    fun depthNeverOverridesAMeasurementTheProfilerDistrusts() {
        for (cls in listOf(RenderClass.DEGRADED, RenderClass.LOW_CONFIDENCE,
                           RenderClass.UNCORRELATED, RenderClass.FLAGGED)) {
            val q = quality(cls)
            val atZero = FlameColors.of(q, false, 0).rgb
            val atFive = FlameColors.of(q, false, 5).rgb
            assertEquals("$cls changed colour with depth — the quality signal is gone",
                         atZero, atFive)
        }
    }

    @Test
    fun anUnclosedFrameKeepsItsOwnColourAtEveryDepth() {
        assertEquals(FlameColors.of(null, true, 0).rgb,
                     FlameColors.of(null, true, 7).rgb)
    }

    // The rainbow must not accidentally reproduce a quality colour, or a
    // trusted frame at some depth would read as a flagged one.
    @Test
    fun noRainbowHueCollidesWithAQualityColour() {
        val reserved = setOf(
            FlameColors.DEGRADED.rgb, FlameColors.LOW_CONFIDENCE.rgb,
            FlameColors.UNCORRELATED.rgb, FlameColors.FLAGGED.rgb,
            FlameColors.UNCLOSED.rgb,
        )
        for (d in 0 until FlameColors.RAINBOW_PERIOD) {
            assertNotEquals("layer $d collides with a quality colour",
                            true, FlameColors.of(null, false, d).rgb in reserved)
        }
    }

    @Test
    fun theCssSurfaceAgreesWithTheSwingOne() {
        for (d in 0 until FlameColors.RAINBOW_PERIOD) {
            val c = FlameColors.of(null, false, d)
            assertEquals("#%02x%02x%02x".format(c.red, c.green, c.blue),
                         FlameColors.cssOf(null, false, d))
        }
    }
}
