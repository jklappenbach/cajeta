package dev.cajeta.idea.profiler

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/**
 * cajeta-profiler 11.1.c — selecting a frame navigates to its source location
 * (spec §8.2).
 *
 * The trace records a location the way the compiler saw it: a path relative to
 * a source root (`tour/Tour.cajeta`), a fully-qualified function name, and a
 * line. The IDE needs a real file. Something has to bridge the two, and getting
 * it wrong is silent — an unresolvable path navigates nowhere and reports
 * nothing, which is precisely how coco shipped a coverage run that loaded
 * cleanly, reported 36.0%, and drew not one gutter (`CocoPathResolverTest`).
 *
 * So both halves are asserted throughout: that resolution SUCCEEDS given the
 * right root, and that it FAILS given none. A test covering only the good case
 * cannot tell a working resolver from one that resolves everything to a
 * constant.
 */
class ProfileNavigationTest {

    @get:Rule
    val tmp: TemporaryFolder = TemporaryFolder()

    private fun loc(file: String, fn: String = "f", line: Int = 10) =
        ProfileSourceLocation(iid = 1, fileName = file, functionName = fn, line = line)

    private fun write(root: File, relative: String): File {
        val f = File(root, relative)
        f.parentFile.mkdirs()
        f.writeText("// $relative\n")
        return f
    }

    // --- the good half -----------------------------------------------------

    @Test
    fun aRelativePathResolvesAgainstTheRootThatHoldsIt() {
        val root = tmp.newFolder("src")
        val expected = write(root, "tour/Tour.cajeta")

        val r = ProfileNavigation.resolve(loc("tour/Tour.cajeta"), listOf(root))
        assertNotNull(r)
        assertEquals(expected.canonicalPath, r!!.file.canonicalPath)
        assertEquals(10, r.line)
        assertTrue(r.exact)
    }

    @Test
    fun rootsAreTriedInOrderAndTheFirstMatchWins() {
        val a = tmp.newFolder("a")
        val b = tmp.newFolder("b")
        val inA = write(a, "tour/Tour.cajeta")
        write(b, "tour/Tour.cajeta")

        val r = ProfileNavigation.resolve(loc("tour/Tour.cajeta"), listOf(a, b))
        assertEquals(inA.canonicalPath, r!!.file.canonicalPath)
    }

    @Test
    fun anAbsolutePathIsUsedAsGiven() {
        val root = tmp.newFolder("src")
        val f = write(root, "tour/Tour.cajeta")

        val r = ProfileNavigation.resolve(loc(f.absolutePath), emptyList())
        assertEquals(f.canonicalPath, r!!.file.canonicalPath)
    }

    // --- the half that keeps it honest -------------------------------------

    @Test
    fun anUnresolvablePathNavigatesNowhereRatherThanSomewhereWrong() {
        val root = tmp.newFolder("src")
        write(root, "tour/Other.cajeta")

        // The file does not exist under any root. Opening `Other.cajeta`
        // because it is the only thing nearby would be worse than opening
        // nothing: the reader would be looking at code that never ran.
        assertNull(ProfileNavigation.resolve(loc("tour/Tour.cajeta"), listOf(root)))
    }

    @Test
    fun aMatchingBasenameInTheWrongPlaceIsNotAMatch() {
        val root = tmp.newFolder("src")
        // Same file name, different package. Basename matching is the tempting
        // shortcut and it lands the reader in the wrong class.
        write(root, "other/Tour.cajeta")

        assertNull(ProfileNavigation.resolve(loc("tour/Tour.cajeta"), listOf(root)))
    }

    @Test
    fun noRootsMeansNoResolution() {
        assertNull(ProfileNavigation.resolve(loc("tour/Tour.cajeta"), emptyList()))
    }

    @Test
    fun aDirectoryIsNotAFile() {
        val root = tmp.newFolder("src")
        File(root, "tour/Tour.cajeta").mkdirs()   // a directory by that name
        assertNull(ProfileNavigation.resolve(loc("tour/Tour.cajeta"), listOf(root)))
    }

    // --- the lambda case, which the tour trace actually contains ------------

    @Test
    fun aFrameWithNoLineOpensTheFileAndSaysTheLineIsNotExact() {
        val root = tmp.newFolder("src")
        write(root, "tour/Tour.cajeta")

        // `tour.Tour.<lambda>` is recorded at line 0 in the fixture. Refusing
        // to navigate would strand a frame that is genuinely in the trace;
        // pretending 0 means line 1 would point at an import.
        val r = ProfileNavigation.resolve(loc("tour/Tour.cajeta", "tour.Tour.<lambda>", 0),
                                          listOf(root))
        assertNotNull(r)
        assertEquals(0, r!!.line)
        assertFalse("a line of 0 is not an exact location", r.exact)
    }

    @Test
    fun editorLinesAreZeroBasedWhereTraceLinesAreOneBased() {
        // OpenFileDescriptor counts from 0 and the trace counts from 1. An
        // off-by-one here puts every frame one line above where it ran, which
        // is close enough to look right and wrong on every single frame.
        assertEquals(136, ProfileNavigation.editorLine(137))
        assertEquals(0, ProfileNavigation.editorLine(1))
        // Line 0 means "no line"; it must not become -1.
        assertEquals(0, ProfileNavigation.editorLine(0))
    }

    // --- against the real trace --------------------------------------------

    @Test
    fun everyLocationInTheTourTraceResolvesUnderItsOwnRoot() {
        val url = javaClass.classLoader.getResource("profiler/tour.pftrace")
        assertNotNull(url)
        val trace = PerfettoTraceReader.read(File(url!!.toURI()).readBytes())
        val locations = trace.events.mapNotNull { it.sourceLocation }.distinctBy { it.iid }
        assertTrue("the fixture should carry source locations", locations.size >= 40)

        // Rebuild the tree the trace describes, then check every one resolves.
        // This is the whole navigation contract in one assertion: a frame the
        // profiler recorded is a frame the IDE can open.
        val root = tmp.newFolder("roots")
        for (l in locations) write(root, l.fileName)

        val unresolved = locations.filter { ProfileNavigation.resolve(it, listOf(root)) == null }
        assertTrue("these locations did not resolve: ${unresolved.map { it.fileName }}",
                   unresolved.isEmpty())

        // And the paths are package-shaped, not bare names — which is what
        // makes basename matching both tempting and wrong.
        assertTrue(locations.any { it.fileName.contains('/') })
    }

    @Test
    fun aStdlibFrameResolvesJustLikeAUserFrame() {
        val url = javaClass.classLoader.getResource("profiler/tour.pftrace")
        val trace = PerfettoTraceReader.read(File(url!!.toURI()).readBytes())
        val stdlib = trace.events.mapNotNull { it.sourceLocation }
            .first { it.fileName.startsWith("cajeta/") }

        val root = tmp.newFolder("roots")
        write(root, stdlib.fileName)
        // §8.9 folds stdlib frames out of the default view; it does not make
        // them unnavigable when a reader unfolds them.
        assertNotNull(ProfileNavigation.resolve(stdlib, listOf(root)))
    }
}
