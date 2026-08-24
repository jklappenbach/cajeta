package dev.cajeta.idea.profiler

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.nio.file.Files

/**
 * cajeta-profiler 11.1.e — selecting a kernel reaches its launching call site
 * (spec §8.4).
 *
 * A kernel runs on a device queue; the line that launched it ran on a host
 * thread, earlier, on a different track. Nothing about the device slice says
 * where it came from except the flow the writer put there: the launch site
 * lists the id (`flow_ids`, field 47) and the device slice terminates it
 * (`terminating_flow_ids`, 48), both `fixed64`.
 *
 * ## Why the link must be the flow and nothing else
 *
 * The two tempting substitutes both work on a small trace and fail on a real
 * one. **By name** breaks the moment a kernel is launched twice — `saxpy` runs
 * four times in this fixture from three different lines. **By time** breaks
 * under exactly the conditions a profiler exists to show: launches are
 * asynchronous, so the device slice that starts next is frequently not the one
 * the most recent launch produced, and overlapping streams have no "nearest"
 * at all.
 *
 * Both would land the developer on a real line of their own code with no
 * indication it was the wrong one — the §8.2 failure mode, one level up.
 *
 * Fixture: `gpu.pftrace`, from `test/error/ProfilerGpuFixtureTests.cpp`.
 * Expected values below come from `trace_processor` reading the same file.
 */
class KernelLaunchSiteTest {

    private fun trace(): ProfileTrace {
        val url = javaClass.classLoader.getResource("profiler/gpu.pftrace")
        assertNotNull("fixture profiler/gpu.pftrace is missing", url)
        return PerfettoTraceReader.read(File(url!!.toURI()).readBytes())
    }

    private fun deviceSlices(t: ProfileTrace): List<ProfileEvent> {
        val queues = t.tracks.filter { it.name.startsWith("queue ") }.map { it.uuid }.toSet()
        return t.events.filter { it.trackUuid in queues && it.isBegin }
    }

    private fun launchSites(t: ProfileTrace): List<ProfileEvent> =
        t.events.filter { it.isInstant && it.flowIds.isNotEmpty() }

    // --- the wire ------------------------------------------------------------

    @Test
    fun theReaderSeesTheFlowsTheWriterEmitted() {
        val t = trace()
        // trace_processor reports 10 flows, each from a distinct launch site to
        // a distinct device slice.
        assertEquals(10, launchSites(t).size)
        assertEquals(10, deviceSlices(t).count { it.terminatingFlowIds.isNotEmpty() })
    }

    @Test
    fun aFlowIdIsReadAsFixed64NotVarint() {
        // flow_ids (47) has a deprecated VARINT twin (36) in the schema. Reading
        // the wrong one parses cleanly and finds no flows at all, so this
        // asserts the values are the launch ids the writer actually minted
        // rather than whatever a mis-parse produced.
        val ids = launchSites(trace()).flatMap { it.flowIds }.sorted()
        assertEquals(10, ids.size)
        assertTrue("flow ids should be small positive launch ids, got $ids",
            ids.all { it in 1..4000 })
        assertEquals("flow ids must be unique", ids.size, ids.distinct().size)
    }

    // --- the link ------------------------------------------------------------

    @Test
    fun everyDeviceSliceReachesExactlyOneLaunchSite() {
        val t = trace()
        val index = KernelLaunchSites.of(t)
        for (slice in deviceSlices(t)) {
            val site = index.siteFor(slice)
            assertNotNull("device slice ${slice.name} reaches no launch site", site)
        }
        assertEquals(10, index.size)
    }

    @Test
    fun theLaunchSiteNamesTheFileAndLineThatLaunchedTheKernel() {
        val t = trace()
        val index = KernelLaunchSites.of(t)
        val slice = deviceSlices(t).first { it.name == "reduce" }
        val loc = index.locationFor(slice)
        assertNotNull("no source location behind the reduce kernel", loc)
        assertEquals("gpu/Reduce.cajeta", loc!!.fileName)
        assertEquals("gpu.Reduce.sum", loc.functionName)
        assertTrue("line should be a real line, was ${loc.line}", loc.line > 0)
    }

    @Test
    fun kernelsSharingANameStillReachTheirOwnLaunchSites() {
        val t = trace()
        val index = KernelLaunchSites.of(t)
        val saxpy = deviceSlices(t).filter { it.name == "saxpy" }
        assertTrue("fixture should launch saxpy more than once", saxpy.size > 1)

        // Same file — they are the same kernel — but the generator gave each
        // launch its own line. Linking by name would collapse these to one.
        val lines = saxpy.mapNotNull { index.locationFor(it)?.line }
        assertEquals(saxpy.size, lines.size)
        assertTrue("every saxpy launch resolved to line ${lines.first()}; " +
            "the kernels were linked by name, not by flow", lines.distinct().size > 1)
    }

    @Test
    fun theLaunchSiteIsOnADifferentTrackFromTheKernel() {
        // If these were on the same track the link would be trivial and the
        // test would prove nothing about flows.
        val t = trace()
        val index = KernelLaunchSites.of(t)
        val slice = deviceSlices(t).first()
        val site = index.siteFor(slice)!!
        assertTrue("kernel and its launch site are on the same track",
            site.trackUuid != slice.trackUuid)
    }

    // --- and when there is no flow -------------------------------------------

    @Test
    fun aSliceWithNoFlowReachesNothingRatherThanGuessing() {
        val index = KernelLaunchSites.of(trace())
        val orphan = ProfileEvent(
            trackUuid = 99, type = PerfettoTraceReader.TYPE_SLICE_BEGIN,
            timestampNs = 1, name = "saxpy", sourceLocation = null,
        )
        // A name that exists in the fixture and a plausible timestamp: both
        // substitutes for the flow would happily return something here.
        assertNull("an unlinked kernel was given a launch site anyway",
            index.siteFor(orphan))
        assertNull(index.locationFor(orphan))
    }

    @Test
    fun anUnknownFlowIdReachesNothing() {
        val index = KernelLaunchSites.of(trace())
        assertNull(index.siteFor(0xDEADBEEFL))
    }

    // --- end to end: from the kernel to an open editor position ---------------

    @Test
    fun theLocationBehindAKernelResolvesToARealFile() {
        val t = trace()
        val index = KernelLaunchSites.of(t)

        // A source tree matching what the trace recorded, the same way
        // ProfileNavigationTest builds one: resolution is about a path under a
        // root, and the fixture's paths are relative by design.
        val root = Files.createTempDirectory("cajeta-gpu-nav").toFile()
        try {
            File(root, "gpu").mkdirs()
            for (f in listOf("Saxpy.cajeta", "Reduce.cajeta", "Transpose.cajeta")) {
                File(root, "gpu/$f").writeText("// $f\n".repeat(80))
            }
            for (slice in deviceSlices(t)) {
                val loc = index.locationFor(slice)
                assertNotNull("no location behind ${slice.name}", loc)
                val resolved = ProfileNavigation.resolve(loc!!, listOf(root))
                assertNotNull("${loc.fileName} did not resolve under the root", resolved)
                assertTrue(resolved!!.file.isFile)
                assertTrue("line should be exact", resolved.exact)
            }
        } finally {
            root.deleteRecursively()
        }
    }

    @Test
    fun aFlameNodeForAKernelCarriesTheFlowThatReachesItsLaunch() {
        // The UI selects a NODE, not a raw event, so the flow has to survive
        // the tree build or §8.4 is one action away from unreachable.
        val t = trace()
        val index = KernelLaunchSites.of(t)
        val queues = FlameGraph.build(t).filter { it.track.name.startsWith("queue ") }
        assertTrue("fixture has no device tracks", queues.isNotEmpty())

        val nodes = queues.flatMap { it.roots }
        assertTrue("no device nodes were built", nodes.isNotEmpty())
        for (n in nodes) {
            assertTrue("flame node ${n.name} lost its terminating flow",
                n.terminatingFlowIds.isNotEmpty())
            assertNotNull("node ${n.name} reaches no launch site", index.siteFor(n))
        }
    }
}
