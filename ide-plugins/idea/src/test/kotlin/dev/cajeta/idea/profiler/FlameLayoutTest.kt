package dev.cajeta.idea.profiler

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * cajeta-profiler 11.2.c/11.2.d — frame geometry (spec §8.3, §8.6).
 *
 * Layout is tested and painting is not, because a rectangle is checkable and a
 * painted pixel is not. Both surfaces — Swing and JCEF — call [FlameLayout], so
 * they cannot drift about where a frame goes; that is the whole reason the
 * geometry lives outside the renderers.
 */
class FlameLayoutTest {

    private fun read(name: String): ProfileTrace {
        val url = javaClass.classLoader.getResource("profiler/$name")
        assertNotNull("fixture profiler/$name is missing", url)
        return PerfettoTraceReader.read(File(url!!.toURI()).readBytes())
    }

    private fun gpu() = ProfileViewModel.of(read("gpu.pftrace"))
    private fun tour() = ProfileViewModel.of(read("tour.pftrace"))

    // --- the shared axis (§8.3) ------------------------------------------------

    @Test
    fun everyFrameIsPlacedOnTheSharedAxisNotItsOwnTrack() {
        val m = gpu()
        val queue = m.tracks.first { it.kind == TrackKind.DEVICE_QUEUE }
        val (rects, _) = FlameLayout.of(m, queue)
        assertTrue("no frames laid out", rects.isNotEmpty())

        // A device queue busy for a fraction of the run must occupy that
        // fraction of the width. Scaled to its own extent it would fill the
        // panel and tell the reader the opposite of what happened.
        val covered = rects.filter { it.depth == 0 }.sumOf { it.width }
        assertTrue("device lane covers the whole axis ($covered)", covered < 1.0)
    }

    @Test
    fun noFrameEscapesTheAxis() {
        val m = tour()
        for (t in m.workTracks) {
            val (rects, _) = FlameLayout.of(m, t)
            for (r in rects) {
                assertTrue("${r.node.name} starts before the axis", r.x >= 0.0)
                assertTrue("${r.node.name} runs past the axis (${r.x} + ${r.width})",
                    r.x + r.width <= 1.0 + 1e-9)
            }
        }
    }

    @Test
    fun aChildIsNeverPlacedAboveItsParent() {
        val m = tour()
        val t = m.workTracks.first { it.name == "cajeta.thread.0" }
        val (rects, _) = FlameLayout.of(m, t)
        val byNode = rects.associateBy { it.node }
        for (r in rects) {
            for (c in r.node.children) {
                val cr = byNode[c] ?: continue
                assertEquals("child is not one row below its parent", r.depth + 1, cr.depth)
                assertTrue("child starts before its parent does", cr.x >= r.x - 1e-9)
            }
        }
    }

    // --- what is dropped, and saying so ------------------------------------------

    @Test
    fun framesTooNarrowToReadAreDroppedAndCounted() {
        val m = tour()
        val t = m.workTracks.first()
        // A threshold coarse enough to drop most of a real tree.
        val (rects, dropped) = FlameLayout.of(m, t, minWidth = 0.5)
        assertTrue("nothing was dropped at a 50% threshold", dropped > 0)
        // Silent truncation reads as "this is everything the trace held".
        val (all, none) = FlameLayout.of(m, t, minWidth = 0.0)
        assertEquals(0, none)
        assertEquals("dropped count does not account for the difference",
            all.size, rects.size + dropped)
    }

    @Test
    fun droppingAFrameDropsItsSubtreeToo() {
        // A child cannot be drawn without its parent's row above it.
        val m = tour()
        val t = m.workTracks.first()
        val (rects, _) = FlameLayout.of(m, t, minWidth = 0.2)
        val kept = rects.map { it.node }.toSet()
        for (r in rects) {
            if (r.depth == 0) continue
            assertTrue("a frame survived while its parent was dropped",
                rects.any { p -> p.node.children.contains(r.node) })
        }
        assertTrue(kept.isNotEmpty() || rects.isEmpty())
    }

    @Test
    fun aZeroSpanModelLaysOutWithoutDividingByZero() {
        val vm = ProfileViewModel.of(ProfileTrace(emptyList(), emptyList(), 0))
        val track = ProfileTrackView(ProfileTrack(1, "x"), TrackKind.THREAD, emptyList(), 0, 0)
        val (rects, dropped) = FlameLayout.of(vm, track)
        assertTrue(rects.isEmpty())
        assertEquals(0, dropped)
    }

    // --- the §8.6 palette ---------------------------------------------------------

    @Test
    fun eachRenderClassGetsItsOwnColour() {
        val colours = RenderClass.entries.map { rc ->
            val q = when (rc) {
                RenderClass.TRUSTED -> MeasurementQuality(ProfileTier.DEVICE, 100, 0)
                RenderClass.LOW_CONFIDENCE -> MeasurementQuality(ProfileTier.DEVICE, 30, 0)
                RenderClass.DEGRADED -> MeasurementQuality(ProfileTier.HOST, 100, 0)
                RenderClass.UNCORRELATED -> MeasurementQuality(ProfileTier.DEVICE, 0, 0)
                RenderClass.FLAGGED -> MeasurementQuality(ProfileTier.DEVICE, 100, 16)
            }
            assertEquals(rc, q.renderClass)
            FlameColors.cssOf(q)
        }
        assertEquals("two render classes share a colour", colours.size, colours.distinct().size)
    }

    @Test
    fun aFlaggedSpanIsHatchedAsWellAsColoured() {
        // Colour alone is invisible to a reader with a colour vision
        // deficiency, and §11.3's is the one distinction that must not be
        // missed.
        assertTrue(FlameColors.hatched(MeasurementQuality(ProfileTier.DEVICE, 100, 16)))
        assertTrue(!FlameColors.hatched(MeasurementQuality(ProfileTier.HOST, 100, 0)))
        assertTrue(!FlameColors.hatched(null))
    }

    @Test
    fun aFrameWithNoQualityRendersAsOrdinaryWork() {
        // Sampled CPU frames have no tier annotation. They are not degraded —
        // they are simply not device measurements — so they must not be greyed
        // out as though something were wrong with them.
        assertEquals(FlameColors.cssOf(MeasurementQuality(ProfileTier.DEVICE, 100, 0)),
            FlameColors.cssOf(null))
    }

    @Test
    fun anUnclosedFrameIsMarkedRegardlessOfQuality() {
        // These were executing when the trace ended, which makes them the
        // answer to "what was it doing when it hung".
        assertTrue(FlameColors.cssOf(null, unclosed = true) != FlameColors.cssOf(null))
    }
}
