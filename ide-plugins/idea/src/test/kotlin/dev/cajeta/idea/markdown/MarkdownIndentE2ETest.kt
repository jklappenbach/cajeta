package dev.cajeta.idea.markdown

import dev.cajeta.idea.markdown.engines.JetBrainsMarkdownEngine
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * End-to-end guard for fenced-code indentation: comment text → stripper →
 * markdown engine → HTML. The unit tests cover the stripper alone; this pins
 * the whole chain, because indentation only has to be dropped at ONE stage for
 * every fenced block in a doc comment to render flat against the margin.
 *
 * The shape is the real one from runtime/src/cajeta/lang/String.cajeta.
 */
class MarkdownIndentE2ETest {

    @Test
    fun fencedIndentationSurvivesIntoTheHtml() {
        val raw = """
            /**
             * ```cajeta
             * if (clean.startsWith("Hello")) {
             *     String loud = clean.toUpperCase();
             * }
             * ```
             */
        """.trimIndent()

        val html = JetBrainsMarkdownEngine().renderToHtml(stripCommentMarkers(raw))

        assertTrue("fenced block should render as <pre>: $html",
            html.contains("<pre>"))
        assertTrue("indentation was lost between comment and HTML: $html",
            html.contains("\n    String loud = clean.toUpperCase();"))
    }
}
