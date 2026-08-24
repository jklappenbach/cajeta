package dev.cajeta.idea.profiler

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * cajeta-profiler 11.2.d — the track tree (spec §8.3).
 *
 * Four queues under one device must read as one device with four lanes, not as
 * four devices. Two of these tests are about files that are not well-formed,
 * because a timeline that hangs or silently loses a lane on a truncated trace
 * is worse than one that draws it flat.
 */
class TrackHierarchyTest {

    private fun view(uuid: Long, name: String, parent: Long = 0) =
        ProfileTrackView(ProfileTrack(uuid, name, parent), TrackKind.of(name), emptyList(), 0, 0)

    private fun gpuModel(): ProfileViewModel {
        val url = javaClass.classLoader.getResource("profiler/gpu.pftrace")
        assertNotNull("fixture profiler/gpu.pftrace is missing", url)
        return ProfileViewModel.of(PerfettoTraceReader.read(File(url!!.toURI()).readBytes()))
    }

    // --- the real shape ---------------------------------------------------------

    @Test
    fun theFixturesQueuesSitUnderTheirDevice() {
        val rows = TrackHierarchy.flatten(gpuModel().tracks)
        val queues = rows.filter { it.view.kind == TrackKind.DEVICE_QUEUE }
        assertTrue("fixture has no device queues", queues.isNotEmpty())
        assertTrue("queues were drawn at the top level, not under their device",
            queues.all { it.depth > 0 })
    }

    @Test
    fun aParentIsDrawnBeforeItsChildren() {
        val rows = TrackHierarchy.flatten(gpuModel().tracks)
        val seen = HashSet<Long>()
        for (r in rows) {
            val parent = r.view.track.parentUuid
            if (parent != 0L && rows.any { it.view.track.uuid == parent }) {
                assertTrue("${r.view.name} was drawn before its parent", parent in seen)
            }
            seen.add(r.view.track.uuid)
        }
    }

    @Test
    fun everyTrackAppearsExactlyOnce() {
        val model = gpuModel()
        val rows = TrackHierarchy.flatten(model.tracks)
        assertEquals(model.tracks.size, rows.size)
        assertEquals(model.tracks.size, rows.map { it.view.track.uuid }.distinct().size)
    }

    // --- files that are not well-formed -------------------------------------------

    @Test
    fun aTrackWhoseParentIsMissingIsDrawnAsARootRatherThanDropped() {
        // Descriptors are written as tracks are discovered, so a trace cut
        // short can hold a queue whose device descriptor never landed.
        // Dropping it would hide the work that ran on it.
        val orphan = view(2, "queue 0", parent = 999)
        val rows = TrackHierarchy.flatten(listOf(orphan))
        assertEquals(1, rows.size)
        assertEquals(0, rows.first().depth)
    }

    @Test
    fun aCycleDegradesToRootsInsteadOfHanging() {
        // No correct writer emits this; a corrupt file can still contain it,
        // and the UI must not spin on it.
        val a = view(1, "context 0", parent = 2)
        val b = view(2, "context 1", parent = 1)
        val rows = TrackHierarchy.flatten(listOf(a, b))
        assertEquals("a cycle lost a track", 2, rows.size)
    }

    @Test
    fun aTrackThatIsItsOwnParentIsARoot() {
        val self = view(5, "queue 9", parent = 5)
        val rows = TrackHierarchy.flatten(listOf(self))
        assertEquals(1, rows.size)
        assertEquals(0, rows.first().depth)
    }

    @Test
    fun anEmptyTraceProducesNoRows() {
        assertTrue(TrackHierarchy.flatten(emptyList()).isEmpty())
    }

    // --- structural tracks are kept ------------------------------------------------

    @Test
    fun aDeviceRowIsKeptEvenThoughItHoldsNoSlices() {
        // It carries the hierarchy. Dropping it leaves queues indented under
        // nothing, which reads as a rendering bug.
        val rows = TrackHierarchy.flatten(gpuModel().tracks)
        assertTrue("the device row was dropped for having no slices of its own",
            rows.any { it.view.kind == TrackKind.DEVICE })
    }
}
