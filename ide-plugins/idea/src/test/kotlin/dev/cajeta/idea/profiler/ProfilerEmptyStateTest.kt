package dev.cajeta.idea.profiler

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * profile-run-history 1.1 (spec §5.4).
 *
 * Opening the profiler tool window showed bare tabs and nothing else: no trace,
 * no explanation, and no way to get one. The only route was
 * Tools -> Cajeta -> "Open Cajeta Profile...", which is not where anyone looks,
 * and the window never said so — `cajeta-profiler` 11.3.a stalled on exactly
 * that (Julian, 2026-08-31: "There's no interface to open a file in the
 * profiler window").
 *
 * The Swing wiring is deliberately thin; what is worth testing is the decision
 * and the words, so both live here rather than inside a component that needs an
 * IDE fixture to instantiate.
 */
class ProfilerEmptyStateTest {

    @Test
    fun theEmptyStateShowsUntilAModelIsLoaded() {
        assertTrue(ProfilerEmptyState.shouldShow(null))
    }

    @Test
    fun aLoadedModelReplacesTheEmptyState() {
        // An EMPTY trace still counts as loaded: the window must then say what
        // the trace contains (nothing), not pretend none was opened.
        val loaded = ProfileViewModel.of(ProfileTrace(emptyList(), emptyList(), 0))
        assertFalse(ProfilerEmptyState.shouldShow(loaded))
    }

    // Both routes, because a developer who profiled from a shell and one who
    // ran from the IDE arrive here with different questions.
    @Test
    fun theMessageNamesBothWaysToGetATrace() {
        val text = ProfilerEmptyState.message().lowercase()
        assertTrue("does not mention opening one: $text", text.contains("open"))
        assertTrue("does not mention a profiled run: $text", text.contains("run"))
        assertTrue("does not name the env var a shell run needs: $text",
                   text.contains("cajeta_profiler"))
    }

    @Test
    fun theMessageIsNotEmptyOrAPlaceholder() {
        val text = ProfilerEmptyState.message()
        assertTrue(text.length > 40)
        assertFalse(text.contains("TODO"))
    }
}
