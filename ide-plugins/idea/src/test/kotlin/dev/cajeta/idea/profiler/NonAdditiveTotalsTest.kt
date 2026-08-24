package dev.cajeta.idea.profiler

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * cajeta-profiler 11.1.g — non-additive intervals are never summed into a total
 * (spec §8.7).
 *
 * A flame graph's totals-by-name view answers "where did the time go", and the
 * honest answer is sometimes "that question has no single number". Two ways an
 * interval sum stops being a duration:
 *
 *  - **Concurrency.** Spans on different tracks overlap in wall time. In the
 *    tour fixture `ParallelDriver.findFailWorker` sums to 189 ms across four
 *    fibers inside a run whose wall clock is 65 ms.
 *  - **Recursion.** A frame nested inside another occurrence of ITSELF has its
 *    time counted once per level. This one hides on a single track, where the
 *    concurrency rule does not fire.
 *
 * Both are true statements about work done and false ones about elapsed time.
 * §8.7 asks that they be presented as relative indicators and never summed into
 * a cost breakdown, so the fraction is withheld and the breakdown that remains
 * sayable is kept — hiding the number entirely would just move the reader to a
 * worse source for it.
 */
class NonAdditiveTotalsTest {

    private fun tour(): List<FlameTrack> {
        val url = javaClass.classLoader.getResource("profiler/tour.pftrace")
        assertNotNull("fixture profiler/tour.pftrace is missing", url)
        return FlameGraph.build(PerfettoTraceReader.read(File(url!!.toURI()).readBytes()))
    }

    private fun totals(): List<FlameTotal> = FlameGraph.byName(tour())

    private fun total(name: String): FlameTotal =
        totals().first { it.name == name }

    // --- the rule fires when it should --------------------------------------

    @Test
    fun aNameSpanningConcurrentTracksOffersNoWallClockFraction() {
        val t = total("cajeta.lang.stream.ParallelDriver.findFailWorker")
        assertTrue("expected occurrences on more than one track", t.byTrack.size > 1)
        // The live instance §8.7 was written for: the sum exceeds the run.
        assertTrue(
            "sum ${t.summedInclusiveNs} should exceed the 64,832,995 ns run",
            t.summedInclusiveNs > 64_832_995L,
        )
        assertNull(
            "a fraction of wall clock was offered for a name that ran on " +
                "${t.byTrack.size} tracks concurrently",
            t.wallClockFraction,
        )
    }

    // --- and not when it should not -----------------------------------------

    @Test
    fun aNameConfinedToOneTrackKeepsItsWallClockFraction() {
        // The rule must not be "never offer a fraction", which would pass every
        // test above while making the totals view useless.
        val single = totals().filter { it.byTrack.size == 1 && it.summedInclusiveNs > 0 }
        assertTrue("fixture has no single-track name to check", single.isNotEmpty())
        for (t in single) {
            assertNotNull(
                "no fraction offered for ${t.name}, which ran on one track only",
                t.wallClockFraction,
            )
        }
    }

    // --- what is kept instead ------------------------------------------------

    @Test
    fun thePerTrackBreakdownAccountsForTheWholeSum() {
        // Withholding the fraction must not lose the measurement. Every
        // nanosecond in the sum is attributable to a track.
        for (t in totals()) {
            assertEquals(
                "per-track breakdown for ${t.name} does not account for its sum",
                t.summedInclusiveNs,
                t.byTrack.values.sum(),
            )
        }
    }

    @Test
    fun noOfferedFractionExceedsTheTrackItIsMeasuredAgainst() {
        // A fraction above 1.0 is the observable symptom of a sum that was not
        // a duration. Nothing in the fixture may produce one.
        for (t in totals()) {
            val f = t.wallClockFraction ?: continue
            assertTrue(
                "${t.name} was reported as ${"%.3f".format(f)} of its track's wall clock",
                f <= 1.0,
            )
        }
    }

    // --- recursion: the case the concurrency rule does not catch -------------

    private fun begin(track: Long, ts: Long, name: String) =
        ProfileEvent(track, PerfettoTraceReader.TYPE_SLICE_BEGIN, ts, name, null)

    private fun end(track: Long, ts: Long) =
        ProfileEvent(track, PerfettoTraceReader.TYPE_SLICE_END, ts, null, null)

    /**
     * `walk` calls itself three deep on ONE track. Constructed rather than
     * taken from a fixture because the tour program does not recurse, and the
     * rule has to hold for programs that do.
     */
    private fun recursive(): ProfileTrace {
        val t = 7L
        return ProfileTrace(
            tracks = listOf(ProfileTrack(t, "cajeta.thread.0")),
            events = listOf(
                begin(t, 1000, "main"),
                begin(t, 1000, "walk"),
                begin(t, 1100, "walk"),
                begin(t, 1200, "walk"),
                end(t, 1300),
                end(t, 1400),
                end(t, 1500),
                end(t, 1600),
            ),
            packetCount = 8,
        )
    }

    @Test
    fun aRecursiveNameIsNotOfferedAsAFractionOfWallClock() {
        val totals = FlameGraph.byName(FlameGraph.build(recursive()))
        val walk = totals.first { it.name == "walk" }

        assertEquals(3, walk.occurrences)
        // 500 + 300 + 100: each level counts the levels beneath it again.
        assertEquals(900L, walk.summedInclusiveNs)
        assertEquals(1, walk.byTrack.size)

        // The track's span is 600 ns. Offering 900/600 = 1.5 would report a
        // frame as 150% of the program's elapsed time, which is the exact
        // failure §8.7 names — and the single-track case is where it hides.
        assertNull(
            "a wall-clock fraction was offered for a self-nested name " +
                "(${walk.summedInclusiveNs} ns summed inside a 600 ns track)",
            walk.wallClockFraction,
        )
    }

    @Test
    fun aNonRecursiveNameOnTheSameTrackStillGetsAFraction() {
        // The recursion rule must key on self-nesting, not on "this track had
        // some recursion somewhere".
        val totals = FlameGraph.byName(FlameGraph.build(recursive()))
        val main = totals.first { it.name == "main" }
        assertEquals(1, main.occurrences)
        assertNotNull("main does not recurse and lost its fraction", main.wallClockFraction)
    }

    @Test
    fun exclusiveTimeStaysAdditiveEvenWhenInclusiveDoesNot() {
        // Exclusive time IS additive under recursion — each nanosecond is
        // charged to exactly one frame — so the sum stays meaningful and is
        // what a totals view should rank by.
        val totals = FlameGraph.byName(FlameGraph.build(recursive()))
        val walk = totals.first { it.name == "walk" }
        // 500-300 + 300-100 + 100 = 200 + 200 + 100
        assertEquals(500L, walk.summedExclusiveNs)
    }
}
