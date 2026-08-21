package dev.cajeta.idea.buildtool

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The Build window colours by stream, and cajeta writes plugin PROGRESS to
 * stderr — so an ordinary coverage run rendered as six consecutive red lines
 * and read as six errors.
 *
 * Both directions are pinned. The one that matters most is the second: a real
 * error must never be painted as normal output, which is a worse failure than
 * the one being fixed.
 */
class ConsoleStreamClassifierTest {

    @Test
    fun pluginProgressOnStderrRendersAsNormalOutput() {
        val progress = listOf(
            "[plugin] coco: [1/6] reference pass (--emit=exe, tree-shake off)",
            "[plugin] coco: [3/6] instrumenting 6 of 10 modules",
            "[plugin] coco: mutation score: 2/3 killed (9 skipped as uncovered)",
            "[incremental] discriminator 561308e6",
            "[cache] hit — re-published build/archive/dev.cajeta.unit-0.2.2.cja",
        )
        for (line in progress) {
            assertTrue("should render as stdout: $line",
                ConsoleStreamClassifier.renderAsStdout(line, stdout = false))
        }
    }

    @Test
    fun aRealErrorOnStderrStaysRed() {
        // The direction that must not regress. Painting one of these white is
        // worse than the red-progress problem this class exists to fix.
        val problems = listOf(
            "cajeta cover: task 'cover' actions[0] (cajeta.coverage.instrument): instrument: llc failed",
            "cajeta: uncaught exception (value=0x3)",
            "  at cajeta.coco.plugin.Pipeline.moduleFiles(Pipeline.cajeta:256)",
            "warning: [plain-return-yields-title] ...",
            "error: unterminated attribute group",
            "cajeta: SIGSEGV caught — fault addr 0x8",
        )
        for (line in problems) {
            assertFalse("should stay on stderr: $line",
                ConsoleStreamClassifier.renderAsStdout(line, stdout = false))
        }
    }

    @Test
    fun aPluginLineThatReportsAProblemStaysRed() {
        // A plugin may log the word "failed". Leave it red rather than deciding
        // we know better than the text.
        assertFalse(ConsoleStreamClassifier.renderAsStdout(
            "[plugin] coco: llc tour/coco/Coupon.ll failed: unterminated attribute group",
            stdout = false))
        assertFalse(ConsoleStreamClassifier.renderAsStdout(
            "[plugin] warning: exclude has no usable reason", stdout = false))
    }

    @Test
    fun stdoutIsAlwaysLeftAlone() {
        assertTrue(ConsoleStreamClassifier.renderAsStdout("anything at all", stdout = true))
        assertTrue(ConsoleStreamClassifier.renderAsStdout("error: even this", stdout = true))
    }

    @Test
    fun unrecognisedStderrIsNotReclassified() {
        // Narrow by design: only known progress markers are re-labelled.
        assertFalse(ConsoleStreamClassifier.renderAsStdout("some tool's chatter", stdout = false))
        assertFalse(ConsoleStreamClassifier.renderAsStdout("", stdout = false))
    }
}
