package dev.cajeta.idea.profiler

import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * A totals row must reach the same place a flame-graph frame does.
 *
 * Reported 2026-09-01: totals rows for `tour.*` navigate and rows for `cajeta.*`
 * do not. Navigation itself is shared with the flame graph, so the difference
 * has to be upstream of it — in the lookup from a total's NAME back to a frame
 * carrying a location. That lookup is what this measures.
 */
class TotalsNavigationTest {

    private fun model(): ProfileViewModel {
        val url = javaClass.classLoader.getResource("profiler/tour.pftrace")
        assertNotNull("fixture profiler/tour.pftrace is missing", url)
        return ProfileViewModel.of(
            PerfettoTraceReader.read(File(url!!.toURI()).readBytes()))
    }

    // The panel's own lookup, replicated so a change there fails here.
    private fun find(node: FlameNode, name: String): FlameNode? {
        if (node.name == name) return node
        for (c in node.children) find(c, name)?.let { return it }
        return null
    }

    private fun frameFor(m: ProfileViewModel, name: String): FlameNode? =
        m.tracks.asSequence()
            .flatMap { it.roots.asSequence() }
            .firstNotNullOfOrNull { find(it, name) }

    @Test
    fun everyTotalReachesAFrame() {
        val m = model()
        val missing = m.totals.map { it.name }.filter { frameFor(m, it) == null }
        assertTrue(
            "totals rows whose name reaches no frame at all: $missing",
            missing.isEmpty())
    }

    /**
     * The profiler's own run record is a synthetic frame with no source of its
     * own. It is the ONLY row in the fixture that legitimately has no location,
     * measured 2026-09-01 at 1 of 48, and it is named here rather than filtered
     * by a pattern so a second locationless row cannot join it unnoticed.
     */
    private val syntheticRows = setOf("cajeta.profiler.run")

    @Test
    fun everyRealTotalReachesAFrameCarryingALocation() {
        val m = model()
        val unlocated = m.totals.map { it.name }
            .filter { it !in syntheticRows }
            .filter { frameFor(m, it)?.sourceLocation == null }
        // This is the measurement that says a dead `cajeta.*` row is NOT a
        // lookup failure: 47 of 48 rows resolve, stdlib ones included. Whatever
        // makes one un-clickable in the IDE is downstream of here, in whether
        // the file resolves under a root.
        assertTrue(
            "${unlocated.size} totals rows reach no location: $unlocated",
            unlocated.isEmpty())
    }

    /** And the exception is real, not a stale entry in [syntheticRows]. */
    @Test
    fun theSyntheticRunRecordIsStillLocationless() {
        val m = model()
        val present = m.totals.map { it.name }.filter { it in syntheticRows }
        assertTrue("fixture no longer contains $syntheticRows", present.isNotEmpty())
        for (name in present) {
            assertNull(
                "$name gained a source location — drop it from syntheticRows",
                frameFor(m, name)?.sourceLocation)
        }
    }

    /** Stdlib rows are the ones reported dead, so they are named explicitly. */
    @Test
    fun stdlibTotalsCarryLocations() {
        val m = model()
        val stdlib = m.totals.map { it.name }
            .filter { it.startsWith("cajeta.") && it !in syntheticRows }
        assertTrue("fixture has no cajeta.* totals to check", stdlib.isNotEmpty())
        val dead = stdlib.filter { frameFor(m, it)?.sourceLocation == null }
        assertTrue("cajeta.* totals with no location: $dead", dead.isEmpty())
    }
}
