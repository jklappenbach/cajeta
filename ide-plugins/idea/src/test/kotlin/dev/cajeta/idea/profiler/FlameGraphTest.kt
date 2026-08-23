package dev.cajeta.idea.profiler

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * cajeta-profiler 11.1.b — the flame graph aggregates into correct inclusive
 * and exclusive time (spec §8.2).
 *
 * Checked against Perfetto's `trace_processor` on the same fixture, so the
 * viewer and the reference implementation agree about where the time went:
 *
 *   inclusive  = slice.dur
 *   exclusive  = slice.dur - sum(children.dur)
 *
 * The numbers below are that query's output for `tour.pftrace`, v57.2,
 * 2026-08-23. They are not round, and that is the point: a tree-walk that is
 * off by one level still produces plausible-looking totals.
 */
class FlameGraphTest {

    private fun trace(): ProfileTrace {
        val url = javaClass.classLoader.getResource("profiler/tour.pftrace")
        assertNotNull("fixture profiler/tour.pftrace is missing", url)
        return PerfettoTraceReader.read(File(url!!.toURI()).readBytes())
    }

    private fun graph(): List<FlameTrack> = FlameGraph.build(trace())

    private fun track(name: String): FlameTrack =
        graph().first { it.track.name == name }

    /** Depth-first search for the first node with this name. */
    private fun find(nodes: List<FlameNode>, name: String): FlameNode? {
        for (n in nodes) {
            if (n.name == name) return n
            find(n.children, name)?.let { return it }
        }
        return null
    }

    private fun flatten(nodes: List<FlameNode>): List<FlameNode> =
        nodes.flatMap { listOf(it) + flatten(it.children) }

    // --- the shape ---------------------------------------------------------

    @Test
    fun everyTrackBecomesATreeWithTheSliceCountTheReferenceReports() {
        val g = graph()
        assertEquals(6, g.size)
        // trace_processor's per-track slice counts. A stack that leaked across
        // tracks would move slices between these and still total 58.
        val counts = g.associate { it.track.name to flatten(it.roots).size }
        assertEquals(36, counts["cajeta.thread.0"])
        assertEquals(12, counts["cajeta.fiber.1"])
        assertEquals(5, counts["cajeta.fiber.2"])
        assertEquals(2, counts["cajeta.fiber.24"])
        assertEquals(2, counts["cajeta.fiber.25"])
        assertEquals(1, counts["cajeta.profiler"])
        assertEquals(58, counts.values.sum())
    }

    @Test
    fun theThreadTrackHasOneRootAndItIsMain() {
        val t = track("cajeta.thread.0")
        assertEquals(1, t.roots.size)
        assertEquals("tour.Tour.main", t.roots[0].name)
        assertEquals(64832995L, t.roots[0].inclusiveNs)
    }

    @Test
    fun simultaneousEventsKeepTheirNestingInsteadOfBeingSortedByTimestamp() {
        // main, forEach and <lambda> all begin at the SAME nanosecond and have
        // the same duration. Their nesting is carried by emission order, not by
        // timestamp, so a builder that sorted by ts would flatten three levels
        // into an arbitrary order and still produce a tree that looked fine.
        val t = track("cajeta.thread.0")
        val main = t.roots[0]
        val forEach = main.children.single()
        assertEquals("cajeta.lang.stream.Stream<tour.DemoClass>.forEach", forEach.name)
        val lambda = forEach.children.single()
        assertEquals("tour.Tour.<lambda>", lambda.name)
        assertEquals(main.inclusiveNs, forEach.inclusiveNs)
        assertEquals(main.inclusiveNs, lambda.inclusiveNs)
    }

    // --- the numbers -------------------------------------------------------

    @Test
    fun inclusiveTimeMatchesTheReferenceImplementation() {
        val t = track("cajeta.thread.0")
        assertEquals(64832995L, find(t.roots, "tour.Tour.main")!!.inclusiveNs)
        assertEquals(1072621L, find(t.roots, "tour.error.ErrorsDemo.execute")!!.inclusiveNs)
        assertEquals(1066719L, find(t.roots, "tour.frame.FrameDemo.execute")!!.inclusiveNs)
        assertEquals(6406836L,
            find(t.roots, "tour.concurrent.ParallelStreamsDemo.execute")!!.inclusiveNs)
        assertEquals(36107333L, find(t.roots, "tour.concurrent.AsyncDemo.execute")!!.inclusiveNs)
    }

    @Test
    fun exclusiveTimeIsInclusiveLessTheDirectChildren() {
        val t = track("cajeta.thread.0")
        // A frame whose only child spans its whole window did none of the work
        // itself. Reporting main's 64 ms as its own cost is the single most
        // misleading thing a flame graph can do.
        val main = find(t.roots, "tour.Tour.main")!!
        assertEquals(64832995L, main.inclusiveNs)
        assertEquals(0L, main.exclusiveNs)

        // And one where the parent genuinely keeps some: trace_processor says
        // inclusive 36107333, exclusive 4254633.
        val async = find(t.roots, "tour.concurrent.AsyncDemo.execute")!!
        assertEquals(36107333L, async.inclusiveNs)
        assertEquals(4254633L, async.exclusiveNs)
    }

    @Test
    fun aLeafSpendsAllOfItsTimeInItself() {
        val leaf = flatten(graph().flatMap { it.roots }).first { it.children.isEmpty() && it.inclusiveNs > 0 }
        assertEquals(leaf.inclusiveNs, leaf.exclusiveNs)
    }

    @Test
    fun theHottestFramesByExclusiveTimeAreTheOnesTheReferenceNames() {
        // trace_processor, ordered by exclusive desc. This is the flame graph's
        // entire purpose — "where did wall time go" — so it is checked against
        // the reference rather than against a shape this code produced.
        val all = flatten(graph().flatMap { it.roots })
            .sortedByDescending { it.exclusiveNs }
            .take(4)
        assertEquals(
            listOf(
                "cajeta.lang.stream.ArrayStream<int32>.next" to 60548355L,
                "cajeta.lang.Optional<int32>.isPresent" to 60548295L,
                "cajeta.lang.stream.ParallelDriver.findFailWorker" to 59486936L,
                "tour.concurrent.AsyncDemo.slowBody" to 48840444L,
            ),
            all.map { it.name to it.exclusiveNs },
        )
    }

    @Test
    fun exclusiveTimeIsNeverNegative() {
        // Children that outlive their parent would produce one, and a negative
        // cost renders as a bar pointing the wrong way rather than as an error.
        for (n in flatten(graph().flatMap { it.roots }))
            assertTrue("${n.name} has exclusive ${n.exclusiveNs}", n.exclusiveNs >= 0)
    }

    // --- §8.7, and the trap this trace actually contains --------------------

    @Test
    fun perNameTotalsCarryTheirTrackBreakdownBecauseFibersOverlap() {
        val g = FlameGraph.build(trace())
        val totals = FlameGraph.byName(g)
        val worker = totals.first { it.name == "cajeta.lang.stream.ParallelDriver.findFailWorker" }

        // trace_processor: 4 occurrences summing to 189,103,673 ns — in a run
        // whose wall clock is 64,832,995 ns. The frames are on four different
        // fibers running concurrently, so the sum is 2.9x the elapsed time of
        // the whole program.
        assertEquals(4, worker.occurrences)
        assertEquals(189103673L, worker.summedInclusiveNs)
        assertTrue("this sum has to exceed the run for the test to mean anything",
                   worker.summedInclusiveNs > 64832995L)

        // §8.7: a sum across concurrent tracks is not a share of wall time and
        // must never be offered as one. The breakdown is what makes it
        // interpretable rather than wrong.
        assertEquals(4, worker.byTrack.size)
        assertEquals(worker.summedInclusiveNs, worker.byTrack.values.sum())
        assertNull("a cross-track sum must not carry a wall-clock fraction",
                   worker.wallClockFraction)
    }

    @Test
    fun aSingleTrackTotalDoesCarryAWallClockFraction() {
        val g = FlameGraph.build(trace())
        val totals = FlameGraph.byName(g)
        // Confined to one track, the sum IS a share of that track's span, and
        // withholding it would be its own kind of dishonesty.
        val main = totals.first { it.name == "tour.Tour.main" }
        assertEquals(1, main.byTrack.size)
        assertNotNull(main.wallClockFraction)
        assertEquals(1.0, main.wallClockFraction!!, 0.0001)
    }

    // --- files that are not pristine ---------------------------------------

    @Test
    fun anUnclosedFrameIsKeptRatherThanDropped() {
        // A trace cut mid-run leaves frames open. They are the frames that were
        // executing when the process died, which makes them the most
        // interesting ones in the file — dropping them would silently remove
        // the answer to "what was it doing when it hung".
        val full = trace()
        val cut = ProfileTrace(
            tracks = full.tracks,
            events = full.events.filter { !it.isEnd },
            packetCount = full.packetCount,
        )
        val g = FlameGraph.build(cut)
        val nodes = flatten(g.flatMap { it.roots })
        assertEquals("every begun frame should survive", 58, nodes.size)
        assertTrue(nodes.any { it.name == "tour.Tour.main" })
        assertTrue("an unclosed frame is marked, not silently zero-length",
                   nodes.first { it.name == "tour.Tour.main" }.unclosed)
    }

    @Test
    fun anEmptyTraceBuildsNoTracksRatherThanThrowing() {
        val g = FlameGraph.build(PerfettoTraceReader.read(ByteArray(0)))
        assertTrue(g.isEmpty())
    }
}
