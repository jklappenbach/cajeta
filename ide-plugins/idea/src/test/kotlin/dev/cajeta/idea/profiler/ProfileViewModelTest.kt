package dev.cajeta.idea.profiler

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * cajeta-profiler 11.2.b — packets mapped to what the views render
 * (spec §8.2, §8.3, §8.5, §8.6).
 *
 * The model is computed once so the flame graph and the timeline agree about
 * the run. Two panels each deriving their own extents is how a device queue
 * ends up drawn against a different axis from the thread that launched into it.
 */
class ProfileViewModelTest {

    private fun read(name: String): ProfileTrace {
        val url = javaClass.classLoader.getResource("profiler/$name")
        assertNotNull("fixture profiler/$name is missing", url)
        return PerfettoTraceReader.read(File(url!!.toURI()).readBytes())
    }

    private fun tour() = ProfileViewModel.of(read("tour.pftrace"))
    private fun gpu() = ProfileViewModel.of(read("gpu.pftrace"))

    // --- track kinds ----------------------------------------------------------

    @Test
    fun tracksAreClassifiedByWhatTheyRepresent() {
        val kinds = tour().tracks.associate { it.name to it.kind }
        assertEquals(TrackKind.THREAD, kinds["cajeta.thread.0"])
        assertEquals(TrackKind.FIBER, kinds["cajeta.fiber.1"])
        assertEquals(TrackKind.PROFILER, kinds["cajeta.profiler"])
    }

    @Test
    fun deviceQueuesAreDistinguishedFromTheDeviceTheyBelongTo() {
        val g = gpu()
        val kinds = g.tracks.associate { it.name to it.kind }
        assertEquals(TrackKind.DEVICE_QUEUE, kinds["queue 0"])
        assertEquals(TrackKind.DEVICE_QUEUE, kinds["queue 1"])
        assertTrue("the fixture's device track was not classified",
            g.tracks.any { it.kind == TrackKind.DEVICE })
    }

    @Test
    fun onlyTracksWithProgramWorkCountAsWorkTracks() {
        // The profiler's own run record and the device/context structure tracks
        // are not lanes of program execution and must not be drawn as though
        // they were.
        val g = gpu()
        assertTrue(g.workTracks.none { it.kind == TrackKind.PROFILER })
        assertTrue(g.workTracks.any { it.kind == TrackKind.DEVICE_QUEUE })
        assertTrue(g.workTracks.any { it.kind == TrackKind.THREAD })
    }

    @Test
    fun aCpuOnlyTraceReportsNoDeviceWork() {
        assertFalse(tour().hasDeviceWork)
        assertTrue(gpu().hasDeviceWork)
    }

    // --- one time axis (§8.3) --------------------------------------------------

    @Test
    fun theAxisSpansEveryWorkTrack() {
        val g = gpu()
        assertTrue("axis has no extent", g.spanNs > 0)
        for (t in g.workTracks) {
            assertTrue("${t.name} starts before the axis does", t.startNs >= g.startNs)
            assertTrue("${t.name} ends after the axis does", t.endNs <= g.endNs)
        }
    }

    @Test
    fun theRunRecordDoesNotDragTheAxisBackToZero() {
        // The profiler's run record is stamped 0 deliberately: it summarizes the
        // run rather than happening at a moment. Including it in the extents
        // would compress every real track into the right-hand edge.
        val g = gpu()
        assertTrue("axis origin was pulled to 0 by a metadata track", g.startNs > 0)
    }

    @Test
    fun aTimestampMapsOntoTheAxisAsAFraction() {
        val g = gpu()
        assertEquals(0.0, g.fractionOf(g.startNs), 1e-9)
        assertEquals(1.0, g.fractionOf(g.endNs), 1e-9)
        // Out-of-range stamps clamp rather than drawing off-panel.
        assertEquals(0.0, g.fractionOf(g.startNs - 1_000_000), 1e-9)
        assertEquals(1.0, g.fractionOf(g.endNs + 1_000_000), 1e-9)
    }

    // --- quality, carried through the tree build -------------------------------

    @Test
    fun aDeviceFrameKeepsItsQualityAfterTheTreeIsBuilt() {
        val g = gpu()
        val queue = g.tracks.first { it.kind == TrackKind.DEVICE_QUEUE }
        val node = queue.roots.first()
        val q = g.qualityOf(node)
        assertNotNull("the tier annotation was lost building the tree", q)
    }

    @Test
    fun aSampledHostFrameHasNoMeasurementQuality() {
        // Same guard as 11.1.f, one layer up: TIER_DEVICE is 0, so this is
        // where a defaulted annotation would promote every CPU frame.
        val t = tour()
        val thread = t.tracks.first { it.kind == TrackKind.THREAD }
        for (n in thread.roots) {
            assertNull("a sampled CPU frame came back as a device measurement",
                t.qualityOf(n))
        }
    }

    @Test
    fun aKernelFrameReachesItsLaunchSiteThroughTheModel() {
        val g = gpu()
        val queue = g.tracks.first { it.kind == TrackKind.DEVICE_QUEUE }
        assertNotNull(g.launchLocationOf(queue.roots.first()))
    }

    // --- instrumentation (§8.5) -------------------------------------------------

    @Test
    fun aTraceWithoutInstrumentationOffersNoCounts() {
        // §8.5 makes the column conditional. Showing an empty one on every
        // sampled profile invites the reader to think the counts are zero.
        val t = tour()
        assertFalse(t.hasInstrumentation)
        assertTrue(t.counts.isEmpty())
    }

    @Test
    fun theInstrumentationWritersNameSuffixIsNormalizedAway() {
        // The sampler writes `Type.method`; the instrumentation writer writes
        // `Type.method(File)`. Matching on the raw string finds nothing for
        // every method in the trace, and does it silently.
        assertEquals("gpu.Saxpy.apply",
            ProfileViewModel.normalizeMethodName("gpu.Saxpy.apply(gpu/Saxpy.cajeta)"))
        assertEquals("gpu.Saxpy.apply",
            ProfileViewModel.normalizeMethodName("gpu.Saxpy.apply"))
    }

    @Test
    fun countsAreLookedUpByTheSameNameTheFlameGraphUses() {
        val counts = mapOf(
            "gpu.Saxpy.apply" to MethodCounts("gpu.Saxpy.apply", "gpu/Saxpy.cajeta", 12, 900, 0))
        val vm = ProfileViewModel(
            trace = ProfileTrace(emptyList(), emptyList(), 0),
            tracks = emptyList(), totals = emptyList(),
            launchSites = KernelLaunchSites.of(ProfileTrace(emptyList(), emptyList(), 0)),
            counts = counts, startNs = 0, endNs = 100,
        )
        val node = FlameNode("gpu.Saxpy.apply", 1, 0, 100, 100, emptyList(), null)
        assertEquals(12L, vm.countsFor(node)?.calls)
        assertNull(vm.countsFor(FlameNode("other", 1, 0, 1, 1, emptyList(), null)))
    }

    // --- degenerate files -------------------------------------------------------

    @Test
    fun anEmptyTraceProducesAModelRatherThanThrowing() {
        val vm = ProfileViewModel.of(ProfileTrace(emptyList(), emptyList(), 0))
        assertTrue(vm.tracks.isEmpty())
        assertEquals(0L, vm.spanNs)
        // A zero-span axis must not divide by zero when something asks where a
        // timestamp sits on it.
        assertEquals(0.0, vm.fractionOf(1234), 1e-9)
    }
}
