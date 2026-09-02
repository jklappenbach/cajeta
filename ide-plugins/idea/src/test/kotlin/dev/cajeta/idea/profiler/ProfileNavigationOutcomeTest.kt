package dev.cajeta.idea.profiler

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * What the window says when a frame click does not land (spec §8.2).
 *
 * `ProfileNavigation.open` returns a Boolean and `CajetaProfilerPanel` dropped
 * it, so a frame that resolved nowhere did nothing and said nothing. Julian,
 * 2026-08-31, on two different frames: "does not have a click. Or clicking it
 * does not take me to code" — the ambiguity is the bug. The code knew which it
 * was and discarded the answer.
 *
 * The tour trace carries three distinct shapes, each needing a different
 * sentence:
 *   cajeta.concurrent.Tasks.withTimeoutInt32 -> cajeta/concurrent/Tasks.cajeta:93
 *                                               (stdlib: outside project roots)
 *   tour.Tour.<lambda>                       -> tour/Tour.cajeta:0
 *                                               (no line; opens at the top)
 *   cajeta.profiler.run                      -> no location at all
 */
class ProfileNavigationOutcomeTest {

    @Test
    fun aFrameWithNoLocationSaysSo() {
        val text = NavigationOutcome.describe(
            frame = "cajeta.profiler.run", location = null, opened = false, exact = false)
        assertTrue(text, text.contains("cajeta.profiler.run"))
        assertTrue("must say the TRACE lacks it, not that the file is missing: $text",
                   text.lowercase().contains("no source location"))
    }

    @Test
    fun anUnresolvedPathNamesThePathAndWhy() {
        val text = NavigationOutcome.describe(
            frame = "cajeta.concurrent.Tasks.withTimeoutInt32",
            location = ProfileSourceLocation(0, "cajeta/concurrent/Tasks.cajeta", "withTimeoutInt32", 93),
            opened = false, exact = true)
        assertTrue("must name the path it could not find: $text",
                   text.contains("cajeta/concurrent/Tasks.cajeta"))
        assertTrue("must say where it looked: $text",
                   text.lowercase().contains("source root"))
    }

    // A lambda opens at the top of the file. If the file is already on screen
    // that is indistinguishable from nothing happening — so it is stated.
    @Test
    fun openingWithoutALineSaysTheLineIsUnknown() {
        val text = NavigationOutcome.describe(
            frame = "tour.Tour.<lambda>",
            location = ProfileSourceLocation(0, "tour/Tour.cajeta", "<lambda>", 0),
            opened = true, exact = false)
        assertTrue(text, text.contains("tour/Tour.cajeta"))
        assertTrue("must say the line was not recorded: $text",
                   text.lowercase().contains("no line"))
    }

    @Test
    fun aCleanNavigationSaysNothing() {
        assertEquals("", NavigationOutcome.describe(
            frame = "tour.Tour.main",
            location = ProfileSourceLocation(0, "tour/Tour.cajeta", "main", 42),
            opened = true, exact = true))
    }

    /**
     * An unmounted stdlib is the one cause that explains the reported shape —
     * project frames navigate, `cajeta.*` frames do not — and it is fixable by
     * the reader, so it is named instead of being folded into a generic
     * "not found".
     */
    @Test
    fun anUnmountedStdlibIsNamedAsTheCause() {
        val msg = NavigationOutcome.describe(
            frame = "cajeta.lang.stream.Stream<tour.DemoClass>.forEach",
            location = ProfileSourceLocation(0, "cajeta/lang/stream/Stream.cajeta", "forEach", 257),
            opened = false,
            exact = false,
            stdlibMounted = false,
        )
        assertTrue("should name the mount: $msg", msg.contains("not mounted"))
        assertTrue("should say where to fix it: $msg", msg.contains("compiler path"))
    }

    /** With the mount present, "not found" must NOT blame it. */
    @Test
    fun aMountedStdlibIsNotBlamedForAMissingFile() {
        val msg = NavigationOutcome.describe(
            frame = "tour.Tour.main",
            location = ProfileSourceLocation(0, "tour/Tour.cajeta", "main", 50),
            opened = false,
            exact = false,
            stdlibMounted = true,
        )
        assertFalse("must not blame the mount: $msg", msg.contains("not mounted"))
        assertTrue("should still say what failed: $msg", msg.contains("tour/Tour.cajeta"))
    }
}
