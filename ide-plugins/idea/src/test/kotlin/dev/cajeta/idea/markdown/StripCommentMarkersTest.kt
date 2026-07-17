package dev.cajeta.idea.markdown

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * Comment-delimiter stripping before markdown rendering. The delimiters must
 * leave nothing behind — `/**` used to survive as a lone `*` (a bullet) and
 * `*/` as a `/`, both of which the renderer then drew as markdown.
 */
class StripCommentMarkersTest {

    @Test
    fun kdocBlockLeavesNoDelimiterArtifacts() {
        val raw = """
            /**
             * First line.
             * Second line.
             */
        """.trimIndent()
        assertEquals("First line.\nSecond line.", stripCommentMarkers(raw))
    }

    @Test
    fun openerDoesNotBecomeABullet() {
        // `/**` alone must vanish, not render as a `*` list item.
        assertEquals("Hello", stripCommentMarkers("/**\n * Hello\n */"))
    }

    @Test
    fun closerDoesNotLeaveASlash() {
        val out = stripCommentMarkers("/**\n * Text\n */")
        assertEquals("Text", out)
        assert(!out.contains("/")) { "closer left a slash: $out" }
    }

    @Test
    fun singleLineBlockComment() {
        assertEquals("Inline.", stripCommentMarkers("/** Inline. */"))
        assertEquals("Plain.", stripCommentMarkers("/* Plain. */"))
    }

    @Test
    fun lineComment() {
        assertEquals("A note.", stripCommentMarkers("// A note."))
    }

    @Test
    fun gutterStripKeepsBoldAndItalic() {
        // The leading `* ` gutter is removed, but `**bold**` / `*italic*` are
        // real markdown and must survive intact.
        assertEquals("**bold** and *italic*",
            stripCommentMarkers("/**\n * **bold** and *italic*\n */"))
    }

    @Test
    fun contentBeforeCloserOnSameLine() {
        assertEquals("Last line.", stripCommentMarkers("/**\n * Last line. */"))
    }
}
