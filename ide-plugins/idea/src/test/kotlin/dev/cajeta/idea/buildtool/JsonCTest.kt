package dev.cajeta.idea.buildtool

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * JSONC preprocessing for `cajeta.json` — mirrors the compiler's
 * `src/cajeta/buildtool/JsonC.h`: strict JSON's data model plus `//` line
 * comments, block comments, and trailing commas.
 *
 * The plugin read the manifest with a strict JSON parser, so any commented
 * manifest — samples/tour/cajeta.json among them — parsed as nothing, and the
 * run configuration silently lost every manifest-derived default.
 *
 * Length is preserved (stripped spans become whitespace) so that any offset a
 * downstream error reports still points at the original source.
 */
class JsonCTest {

    private fun strip(s: String) = CajetaManifest.stripJsonComments(s)

    @Test
    fun stripsLineComments() {
        val src = "{\n  // a comment\n  \"a\": 1\n}"
        assertEquals(src.length, strip(src).length)
        assertEquals("""{"a":1}""", strip(src).filterNot { it.isWhitespace() })
    }

    @Test
    fun stripsBlockComments() {
        val src = "{ /* one\n   two */ \"a\": 1 }"
        assertEquals(src.length, strip(src).length)
        assertEquals("""{"a":1}""", strip(src).filterNot { it.isWhitespace() })
    }

    @Test
    fun stripsTrailingCommasInObjectsAndArrays() {
        assertEquals("""{"a":[1,2]}""",
            strip("""{"a": [1, 2, ], }""").filterNot { it.isWhitespace() })
    }

    // The trap: a `//` inside a STRING is data, not a comment. A naive
    // stripper eats the rest of every line holding a URL.
    @Test
    fun doesNotTouchSlashesInsideStrings() {
        val src = """{"url": "https://example.com/x", "b": 2}"""
        assertEquals(src, strip(src))
    }

    @Test
    fun doesNotTouchBlockCommentMarkersInsideStrings() {
        val src = """{"s": "a /* not a comment */ b"}"""
        assertEquals(src, strip(src))
    }

    // An escaped quote must not be read as closing the string, or the
    // stripper loses track of where the string ends.
    @Test
    fun handlesEscapedQuotesInStrings() {
        val src = """{"s": "he said \"hi\" // not a comment", "b": 2}"""
        assertEquals(src, strip(src))
    }

    // A comma inside a string is not a trailing comma.
    @Test
    fun doesNotTouchCommasInsideStrings() {
        val src = """{"s": "a, b, ", "c": 1}"""
        assertEquals(src, strip(src))
    }

    @Test
    fun leavesStrictJsonUnchanged() {
        val src = """{"a":1,"b":[2,3],"c":{"d":"e"}}"""
        assertEquals(src, strip(src))
    }

    // The whole point: the real tour manifest shape must now parse.
    @Test
    fun parsesACommentedManifest() {
        val src = """
            {
                // Cajeta language tour — built with the cajeta build tool.
                //   cajeta build     → build/exe/tour
                "settings": {
                    "build": {
                        "entry-method": "tour.Main::main",  // the entry
                        "source-root": "src/main/cajeta",
                    },
                },
            }
        """.trimIndent()
        val b = CajetaManifest.parseBuildSettings(src)
        assertEquals("tour.Main.main", b.entryMethod)
        assertEquals("src/main/cajeta", b.sourceRoot)
    }

    // The regression: an earlier stripper treated the comma in `["a", "b"]` as
    // trailing and blanked it, yielding invalid JSON. tools/mcp's manifest has
    // exactly this shape ("capabilities": ["filesystem", "network"]).
    @Test
    fun keepsSeparatorCommasBetweenStringsInArrays() {
        val src = """{"caps": ["filesystem", "network"], "a": 1}"""
        assertEquals(src, strip(src))
    }

    @Test
    fun keepsSeparatorCommasBetweenObjectsInArrays() {
        val src = """[{"a":1}, {"b":2}]"""
        assertEquals(src, strip(src))
    }
}
