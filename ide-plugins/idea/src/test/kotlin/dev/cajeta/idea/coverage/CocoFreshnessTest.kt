package dev.cajeta.idea.coverage

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.nio.file.Files

/**
 * ide-coverage-plan Unit 5.1 — per-file freshness.
 *
 * Coverage drawn against source that has since changed is worse than none: it is
 * confidently wrong (spec §5). Freshness is per FILE rather than per project
 * because the common case is one file edited out of many, and blanking the whole
 * display for that would make the feature useless exactly when it is being used.
 *
 * The rule these pin: **a file is fresh iff its content is what the run
 * measured.** Modification time alone is not enough in either direction, and
 * both failure modes are tested.
 */
class CocoFreshnessTest {

    private lateinit var root: File
    private lateinit var profile: File

    private fun run(vararg files: String): CocoFreshness {
        root = Files.createTempDirectory("coco-fresh").toFile()
        File(root, "run").mkdirs()
        profile = File(root, "run/coco.profile").also { it.writeText("coco-profile v1\n") }
        val paths = files.map { name ->
            File(root, name).also { it.parentFile.mkdirs(); it.writeText("// $name\nline two\n") }
        }
        // The run happened after the sources were written.
        val runAt = paths.maxOf { it.lastModified() } + 10_000
        profile.setLastModified(runAt)
        val f = CocoFreshness()
        f.observeRun(profile, paths.map { it.absolutePath })
        return f
    }

    private fun edit(name: String, text: String) {
        val f = File(root, name)
        val was = f.lastModified()
        f.writeText(text)
        f.setLastModified(was + 60_000)
    }

    // --- 5.1.a  source modified after a run is stale -------------------------

    @Test
    fun aFileEditedAfterTheRunIsStale() {
        val f = run("A.cajeta", "B.cajeta")
        assertEquals(CocoFreshness.State.FRESH, f.stateOf(File(root, "A.cajeta").absolutePath))
        edit("A.cajeta", "// A.cajeta\nline two\nline three\n")
        assertEquals(CocoFreshness.State.STALE, f.stateOf(File(root, "A.cajeta").absolutePath))
        assertNotNull("says why", f.reasonFor(File(root, "A.cajeta").absolutePath))
    }

    // --- 5.1.c  an unchanged file in a partly-changed project keeps markings --

    @Test
    fun anUnchangedFileStaysFreshWhenItsNeighbourChanges() {
        val f = run("A.cajeta", "B.cajeta")
        edit("A.cajeta", "// A.cajeta\ncompletely different\n")
        assertEquals(CocoFreshness.State.STALE, f.stateOf(File(root, "A.cajeta").absolutePath))
        assertEquals(
            "B was not touched and its coverage is still true",
            CocoFreshness.State.FRESH,
            f.stateOf(File(root, "B.cajeta").absolutePath),
        )
        assertEquals(listOf(File(root, "A.cajeta").absolutePath), f.staleFiles())
    }

    // --- modification time alone is wrong in BOTH directions -----------------

    @Test
    fun aTouchThatChangesNothingDoesNotGoStale() {
        // The IDE saves constantly. If a bumped mtime alone meant stale, every
        // file would go stale within seconds of loading a run and the feature
        // would be unusable — so content, not the clock, is the test.
        val f = run("A.cajeta")
        val a = File(root, "A.cajeta")
        a.setLastModified(a.lastModified() + 120_000)
        assertEquals(CocoFreshness.State.FRESH, f.stateOf(a.absolutePath))
    }

    @Test
    fun anEditThatIsUndoneReturnsToFresh() {
        val f = run("A.cajeta")
        val a = File(root, "A.cajeta")
        val original = a.readText()
        edit("A.cajeta", "// changed\n")
        assertEquals(CocoFreshness.State.STALE, f.stateOf(a.absolutePath))
        edit("A.cajeta", original)
        assertEquals(
            "the content is once again what the run measured",
            CocoFreshness.State.FRESH,
            f.stateOf(a.absolutePath),
        )
    }

    @Test
    fun aFileAlreadyChangedBeforeTheResultsWereLoadedIsStaleNotTrusted() {
        // Snapshotting at load time can only be trusted when the file still
        // predates the run. A file edited between the run finishing and the
        // results being opened was measured in a state we never saw, so it must
        // not be silently adopted as the measured content.
        root = Files.createTempDirectory("coco-fresh-late").toFile()
        File(root, "run").mkdirs()
        profile = File(root, "run/coco.profile").also { it.writeText("coco-profile v1\n") }
        val a = File(root, "A.cajeta").also { it.writeText("// A\n") }
        profile.setLastModified(a.lastModified() - 60_000)   // run is OLDER than the source

        val f = CocoFreshness()
        f.observeRun(profile, listOf(a.absolutePath))
        assertEquals(CocoFreshness.State.STALE, f.stateOf(a.absolutePath))
        assertTrue(
            "explains that the file post-dates the run: ${f.reasonFor(a.absolutePath)}",
            f.reasonFor(a.absolutePath)!!.contains("changed"),
        )
    }

    @Test
    fun aDeletedFileIsStaleRatherThanFresh() {
        val f = run("A.cajeta")
        val a = File(root, "A.cajeta")
        assertTrue(a.delete())
        assertEquals(CocoFreshness.State.STALE, f.stateOf(a.absolutePath))
    }

    @Test
    fun aFileTheRunNeverMentionedIsUnknownNotFresh() {
        // Reporting FRESH for a file with no coverage would claim the run said
        // something about it.
        val f = run("A.cajeta")
        assertEquals(CocoFreshness.State.UNKNOWN, f.stateOf(File(root, "Z.cajeta").absolutePath))
    }

    // --- 5.1.d  age and origin are discoverable ------------------------------

    @Test
    fun theRunsAgeAndOriginAreAvailable() {
        val f = run("A.cajeta")
        assertEquals(profile, f.origin)
        assertEquals(profile.lastModified(), f.runTimestamp)
        assertTrue("describes itself: ${f.describeRun()}", f.describeRun()!!.contains("coco.profile"))
    }

    @Test
    fun aFreshnessWithNoRunObservedReportsNothingRatherThanGuessing() {
        val f = CocoFreshness()
        assertNull(f.origin)
        assertNull(f.runTimestamp)
        assertNull(f.describeRun())
        assertEquals(CocoFreshness.State.UNKNOWN, f.stateOf("/anywhere/A.cajeta"))
        assertTrue(f.staleFiles().isEmpty())
    }

    @Test
    fun clearingForgetsTheRun() {
        val f = run("A.cajeta")
        f.clear()
        assertNull(f.origin)
        assertEquals(CocoFreshness.State.UNKNOWN, f.stateOf(File(root, "A.cajeta").absolutePath))
    }
}
