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

    // Indentation inside a fenced block is CONTENT, not scaffolding. The
    // stripper used to run line.trim(), which flattened every fenced block to
    // the left margin — the real case is runtime/src/cajeta/lang/String.cajeta.
    @Test
    fun fencedCodeKeepsItsIndentation() {
        val raw = """
            /**
             * ```cajeta
             * if (clean.startsWith("Hello")) {
             *     String loud = clean.toUpperCase();
             * }
             * ```
             */
        """.trimIndent()
        assertEquals(
            "```cajeta\n" +
            "if (clean.startsWith(\"Hello\")) {\n" +
            "    String loud = clean.toUpperCase();\n" +
            "}\n" +
            "```",
            stripCommentMarkers(raw))
    }

    // Only ONE space after the gutter is scaffolding; deeper nesting survives
    // at full depth.
    @Test
    fun deeperNestingKeepsEveryLevel() {
        val out = stripCommentMarkers("/**\n * a\n *     b\n *         c\n */")
        assertEquals("a\n    b\n        c", out)
    }

    // The same rule for `//` runs: `// ` is the gutter, the rest is content.
    @Test
    fun lineCommentKeepsIndentationPastTheGutter() {
        assertEquals("a\n    b", stripCommentMarkers("// a\n//     b"))
    }

    // A blank gutter line inside a fence stays blank rather than vanishing.
    @Test
    fun blankGutterLineSurvivesAsBlank() {
        assertEquals("```\na\n\nb\n```",
            stripCommentMarkers("/**\n * ```\n * a\n *\n * b\n * ```\n */"))
    }
}
