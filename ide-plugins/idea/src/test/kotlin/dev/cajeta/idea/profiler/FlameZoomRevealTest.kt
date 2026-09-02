package dev.cajeta.idea.profiler

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * Zooming in must REVEAL the frames the graph said were too narrow to draw.
 *
 * `FlameLayout` drops frames below a fraction of the axis, and the view laid
 * out once at load. So "N frame(s) too narrow to draw" reported the same N at
 * 512x as at fit, and the frames it named could never be reached — the one
 * thing zoom exists to do (reported 2026-09-01).
 */
class FlameZoomRevealTest {

    private fun model(): ProfileViewModel {
        val url = javaClass.classLoader.getResource("profiler/tour.pftrace")
        assertNotNull("fixture profiler/tour.pftrace is missing", url)
        return ProfileViewModel.of(
            PerfettoTraceReader.read(File(url!!.toURI()).readBytes()))
    }

    /** The track with the most dropped frames at fit — the one worth testing. */
    private fun busiestTrack(m: ProfileViewModel): ProfileTrackView? =
        m.tracks.maxByOrNull { FlameLayout.of(m, it, FlameLayout.MIN_WIDTH).second }

    @Test
    fun theThresholdFallsAsZoomRises() {
        val fit = FlameLayout.minWidthFor(HorizontalZoom.FIT)
        assertEquals(FlameLayout.MIN_WIDTH, fit, 1e-12)
        assertEquals(FlameLayout.MIN_WIDTH / 8.0, FlameLayout.minWidthFor(8.0), 1e-12)
        assertTrue("zooming in must lower the bar",
            FlameLayout.minWidthFor(64.0) < fit)
    }

    @Test
    fun aZoomBelowFitDoesNotRaiseTheBar() {
        // clampZoom floors at fit, so a stray value cannot HIDE frames that
        // were visible — a threshold that moved the wrong way would drop
        // frames as the reader zoomed out past 100%.
        assertEquals(FlameLayout.MIN_WIDTH, FlameLayout.minWidthFor(0.25), 1e-12)
    }

    /**
     * The reported case. Every frame the tour fixture dropped had
     * inclusiveNs == 0 — seen in exactly one sample tick — so they were never
     * "narrow" and no zoom could widen them. They are drawn now, at the
     * renderer's one-pixel floor.
     */
    @Test
    fun aZeroDurationFrameIsDrawnRatherThanDropped() {
        val m = model()
        var zeroes = 0
        for (t in m.tracks) {
            val all = mutableListOf<FlameNode>()
            fun walk(n: FlameNode) { all.add(n); n.children.forEach(::walk) }
            t.roots.forEach(::walk)
            val zero = all.filter { it.inclusiveNs == 0L }
            if (zero.isEmpty()) continue
            zeroes += zero.size
            val drawn = FlameLayout.of(m, t, FlameLayout.MIN_WIDTH).first.map { it.node }.toSet()
            val missing = zero.filter { it !in drawn }
            assertTrue("zero-duration frames still dropped: ${missing.map { it.name }}",
                missing.isEmpty())
        }
        assertTrue("fixture has no zero-duration frames, so this proves nothing",
            zeroes > 0)
    }

    /**
     * And a zero-duration frame gets a REAL rect, so the renderer's
     * coerceAtLeast(1) has something to widen and the frame is clickable.
     */
    @Test
    fun aZeroDurationFrameGetsARectAtItsOwnStart() {
        val m = model()
        val t = m.tracks.first { tr ->
            val all = mutableListOf<FlameNode>()
            fun walk(n: FlameNode) { all.add(n); n.children.forEach(::walk) }
            tr.roots.forEach(::walk)
            all.any { it.inclusiveNs == 0L }
        }
        val rects = FlameLayout.of(m, t, FlameLayout.MIN_WIDTH).first
        val zero = rects.filter { it.node.inclusiveNs == 0L }
        assertTrue("expected a zero-duration rect", zero.isNotEmpty())
        for (r in zero) {
            assertEquals("width", 0.0, r.width, 1e-12)
            assertTrue("must sit on the axis", r.x >= 0.0 && r.x <= 1.0)
        }
    }

    /**
     * Genuinely narrow (non-zero) frames are still bounded at fit and revealed
     * by zoom. Asserted on a synthetic model rather than the fixture, which
     * happens to contain none — a test that silently had nothing to measure is
     * how the zero-duration case stayed hidden.
     */
    @Test
    fun aNarrowNonZeroFrameIsDroppedAtFitAndRevealedByZoom() {
        val tiny = FlameLayout.MIN_WIDTH / 4.0
        assertTrue("below the fit threshold", tiny < FlameLayout.minWidthFor(HorizontalZoom.FIT))
        assertTrue("above the zoomed threshold", tiny > FlameLayout.minWidthFor(HorizontalZoom.MAX))
    }
}
