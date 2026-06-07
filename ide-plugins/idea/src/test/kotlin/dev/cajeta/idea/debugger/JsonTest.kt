package dev.cajeta.idea.debugger

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/** Unit tests for the hand-rolled JSON DOM that backs the DAP wire layer. */
class JsonTest {

    @Test
    fun serializesPrimitives() {
        assertEquals("1", Json.of(1).toCompactString())
        assertEquals("9000000000", Json.of(9_000_000_000L).toCompactString())
        assertEquals("true", Json.of(true).toCompactString())
        assertEquals("false", Json.of(false).toCompactString())
        assertEquals("null", Json.Null.toCompactString())
        assertEquals("\"hi\"", Json.of("hi").toCompactString())
    }

    @Test
    fun serializesObjectInInsertionOrder() {
        val o = Json.obj(
            "seq" to Json.of(3),
            "type" to Json.of("request"),
            "command" to Json.of("stackTrace"),
        )
        assertEquals("""{"seq":3,"type":"request","command":"stackTrace"}""", o.toCompactString())
    }

    @Test
    fun serializesNestedArraysAndObjects() {
        val body = Json.obj(
            "stackFrames" to Json.arr(
                Json.obj("id" to Json.of(0), "line" to Json.of(6)),
            ),
            "totalFrames" to Json.of(1),
        )
        assertEquals(
            """{"stackFrames":[{"id":0,"line":6}],"totalFrames":1}""",
            body.toCompactString(),
        )
    }

    @Test
    fun escapesStringSpecials() {
        val s = Json.of("a\"b\\c\nd\te")
        // " \ \n \t must be escaped; the rest pass through.
        assertEquals(""""a\"b\\c\nd\te"""", s.toCompactString())
    }

    @Test
    fun roundTripsThroughParse() {
        val original = Json.obj(
            "type" to Json.of("response"),
            "request_seq" to Json.of(7),
            "success" to Json.of(true),
            "body" to Json.obj(
                "variables" to Json.arr(
                    Json.obj(
                        "name" to Json.of("a"),
                        "value" to Json.of("6"),
                        "type" to Json.of("int32"),
                    ),
                ),
            ),
        )
        val text = original.toCompactString()
        val reparsed = Json.parse(text)
        assertEquals(text, reparsed.toCompactString())
        assertEquals("a", reparsed.at("body").at("variables")[0].at("name").asString())
        assertEquals(7, reparsed.at("request_seq").asInt())
        assertTrue(reparsed.at("success").asBool())
    }

    @Test
    fun parsesWhitespaceAndOpaqueValueStrings() {
        val text = """ { "value" : "<demo.Foo@0x7ffd12ab>" , "ref" : 0 } """
        val j = Json.parse(text)
        assertEquals("<demo.Foo@0x7ffd12ab>", j.at("value").asString())
        assertEquals(0, j.at("ref").asInt())
    }

    @Test
    fun parsesUnicodeEscape() {
        // Build the JSON text "A" without an adjacent backslash-u in
        // source (keeps the test robust against escape normalization).
        val backslash = '\\'
        val text = "\"" + backslash + "u0041\""
        assertEquals("A", Json.parse(text).asString())
    }

    @Test
    fun parsesNegativeAndFractionalNumbers() {
        assertEquals(-7, Json.parse("-7").asInt())
        assertEquals(2.25, Json.parse("2.25").asDouble(), 0.0)
    }

    @Test
    fun emptyContainers() {
        assertEquals("{}", Json.obj().toCompactString())
        assertEquals("[]", Json.arr().toCompactString())
        assertEquals(0, Json.parse("[]").size)
        assertEquals(0, Json.parse("{}").size)
    }

    @Test
    fun optReturnsNullForMissingKeyAndNonObject() {
        assertEquals(null, Json.obj().opt("missing"))
        assertEquals(null, Json.of(5).opt("any"))
    }

    @Test
    fun malformedInputThrows() {
        try {
            Json.parse("""{"a":}""")
            fail("expected parse failure")
        } catch (e: IllegalArgumentException) {
            // expected
        }
    }
}
