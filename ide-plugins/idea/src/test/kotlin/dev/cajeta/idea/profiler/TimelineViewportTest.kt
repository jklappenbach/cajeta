package dev.cajeta.idea.profiler

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The timeline had no scrollbars: its canvas is a plain JPanel, which neither
 * reports a scrollable height nor stretches to a viewport wider than itself, so
 * a long track list was clipped and a wide tool window painted into a fixed
 * 800 px column (reported 2026-09-01).
 *
 * Both arms of each decision are asserted. A policy that always returns true is
 * as broken as one that always returns false — it just fails the other way, and
 * only one of the two shows up as "no scrollbar".
 */
class TimelineViewportTest {

    @Test
    fun contentHeightGrowsWithTrackCount() {
        val one = TimelineViewport.contentHeight(1, 22, 18, 8)
        val thirty = TimelineViewport.contentHeight(30, 22, 18, 8)
        assertEquals(22 + 18 + 8, one)
        assertEquals(30 * 22 + 18 + 8, thirty)
        // The whole point: more tracks must ask for more room, or the viewport
        // has no reason to offer a scrollbar.
        assertTrue("more tracks must need more height", thirty > one)
    }

    @Test
    fun anEmptyTimelineStillReservesTheRuler() {
        assertEquals(18 + 8, TimelineViewport.contentHeight(0, 22, 18, 8))
    }

    @Test
    fun aWideViewportIsFilledRatherThanLeftBlank() {
        assertTrue(TimelineViewport.tracksViewportWidth(1200, 510))
        assertTrue("exactly the minimum still fills",
            TimelineViewport.tracksViewportWidth(510, 510))
    }

    @Test
    fun aNarrowViewportScrollsRatherThanCrushingTheLanes() {
        assertFalse(TimelineViewport.tracksViewportWidth(509, 510))
        assertFalse(TimelineViewport.tracksViewportWidth(200, 510))
    }

    @Test
    fun aShortTrackListFillsTheViewportHeight() {
        assertTrue(TimelineViewport.tracksViewportHeight(800, 200))
        assertTrue(TimelineViewport.tracksViewportHeight(200, 200))
    }

    @Test
    fun aLongTrackListScrollsVertically() {
        assertFalse(TimelineViewport.tracksViewportHeight(200, 201))
        assertFalse(TimelineViewport.tracksViewportHeight(300, 1400))
    }
}
