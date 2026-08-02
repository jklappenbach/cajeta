package dev.cajeta.idea.jsonl

import dev.cajeta.idea.debugger.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * json-viewer Unit 5 (spec §2.1.5, §4.1.4, §5.1.3; format decision 1.3.1a).
 * Vectors are taken from RFC 8949 Appendix A where they exist, so the codec is
 * checked against the standard rather than against itself.
 */
class CborCodecTest {

    private fun hex(s: String): ByteArray =
        s.replace(" ", "").chunked(2).map { it.toInt(16).toByte() }.toByteArray()

    private fun toHex(b: ByteArray): String =
        b.joinToString(" ") { String.format("%02x", it) }

    private fun ok(bytes: ByteArray): BinaryDecodeResult.Ok =
        CborCodec.decode(bytes) as BinaryDecodeResult.Ok

    private fun one(bytes: ByteArray): Json = ok(bytes).items.single()

    // --- 5.1.1 the major types, against RFC 8949 Appendix A -----------------

    @Test
    fun rfcVectorsDecode() {
        assertEquals(0.0, (one(hex("00")) as Json.Num).value, 0.0)
        assertEquals(1.0, (one(hex("01")) as Json.Num).value, 0.0)
        assertEquals(10.0, (one(hex("0a")) as Json.Num).value, 0.0)
        assertEquals(23.0, (one(hex("17")) as Json.Num).value, 0.0)
        assertEquals(24.0, (one(hex("1818")) as Json.Num).value, 0.0)
        assertEquals(1000.0, (one(hex("1903e8")) as Json.Num).value, 0.0)
        assertEquals(1000000.0, (one(hex("1a000f4240")) as Json.Num).value, 0.0)
        assertEquals(-1.0, (one(hex("20")) as Json.Num).value, 0.0)
        assertEquals(-1000.0, (one(hex("3903e7")) as Json.Num).value, 0.0)
        assertEquals(1.1, (one(hex("fb3ff199999999999a")) as Json.Num).value, 1e-12)
        assertEquals(Json.Bool(false), one(hex("f4")))
        assertEquals(Json.Bool(true), one(hex("f5")))
        assertEquals(Json.Null, one(hex("f6")))
        assertEquals("", (one(hex("60")) as Json.Str).value)
        assertEquals("IETF", (one(hex("6449455446")) as Json.Str).value)
        assertEquals("ü", (one(hex("62c3bc")) as Json.Str).value)
    }

    @Test
    fun arraysAndMapsDecode() {
        // [1, 2, 3]
        val arr = one(hex("83010203")) as Json.Arr
        assertEquals(3, arr.items.size)
        // {"a": 1, "b": [2, 3]}
        val obj = one(hex("a26161016162820203")) as Json.Obj
        assertEquals(2, obj.entries.size)
        assertEquals(1.0, (obj.entries["a"] as Json.Num).value, 0.0)
        assertEquals(2, (obj.entries["b"] as Json.Arr).items.size)
        // nested + empty containers
        assertEquals(0, (one(hex("80")) as Json.Arr).items.size)
        assertEquals(0, (one(hex("a0")) as Json.Obj).entries.size)
        val nested = one(hex("8301820203820405")) as Json.Arr
        assertEquals(3, nested.items.size)
    }

    // --- 5.1.2 deterministic re-encode -------------------------------------

    @Test
    fun canonicalInputReEncodesByteIdentically() {
        // The §4.1.4 promise: an unedited round-trip changes nothing.
        for (vector in listOf("00", "1903e8", "20", "6449455446", "83010203",
                              "a26161016162820203", "f4", "f5", "f6",
                              "fb3ff199999999999a")) {
            val bytes = hex(vector)
            val decoded = ok(bytes)
            assertTrue("$vector should be canonical", decoded.editable)
            assertEquals("round-trip of $vector",
                toHex(bytes), toHex(CborCodec.encode(decoded.items.single())))
        }
    }

    @Test
    fun mapKeysAreEmittedInDeterministicOrder() {
        // §4.2.1 orders by ENCODED key: length first, then bytewise — not the
        // insertion order the tree happens to hold.
        val obj = Json.Obj()
        obj.entries["bb"] = Json.of(2)
        obj.entries["a"] = Json.of(1)
        assertEquals("a2 61 61 01 62 62 62 02", toHex(CborCodec.encode(obj)))
    }

    @Test
    fun nonShortestEncodingIsReadableButNotEditable() {
        // 1 written in two bytes instead of one: valid CBOR, not deterministic.
        val decoded = ok(hex("1801"))
        assertEquals(1.0, (decoded.items.single() as Json.Num).value, 0.0)
        assertFalse(decoded.editable)
        assertNotNull(decoded.reason)
    }

    @Test
    fun indefiniteLengthIsReadableButNotEditable() {
        // (_ 1, 2, 3) — a streamed array, re-encodable only as definite.
        val decoded = ok(hex("9f010203ff"))
        assertEquals(3, (decoded.items.single() as Json.Arr).items.size)
        assertFalse(decoded.editable)
        // ...and the streamed text form, which arrives in chunks.
        val text = ok(hex("7f657374726561646d696e67ff"))
        assertEquals("streaming", (text.items.single() as Json.Str).value)
        assertFalse(text.editable)
    }

    @Test
    fun byteStringsReadAsBase64AndLockTheFile() {
        // No exact JSON form, so the file stays readable and read-only rather
        // than being silently rewritten as text on save.
        val decoded = ok(hex("4401020304"))
        assertEquals("AQIDBA==", (decoded.items.single() as Json.Str).value)
        assertFalse(decoded.editable)
    }

    @Test
    fun hugeIntegersAreReadableButNotEditable() {
        // 2^63-1 cannot survive a Double-backed model; better read-only than
        // quietly rounding somebody's identifier.
        val decoded = ok(hex("1b7fffffffffffffff"))
        assertFalse(decoded.editable)
        assertTrue(decoded.reason!!.contains("exact double precision"))
    }

    // --- 5.1.3 CBOR Sequences (RFC 8742) -----------------------------------

    @Test
    fun aSequenceDecodesToOneItemPerRecord() {
        // Three documents back to back: the binary shape of a JSONL stream.
        val seq = hex("a1616101") + hex("a1616102") + hex("a1616103")
        val decoded = ok(seq)
        assertEquals(3, decoded.items.size)
        assertEquals(3.0, ((decoded.items[2] as Json.Obj).entries["a"] as Json.Num).value, 0.0)
        assertTrue(decoded.editable)
        assertEquals(toHex(seq), toHex(CborCodec.encodeSequence(decoded.items)))
    }

    @Test
    fun theSelfDescribeHeaderIsAccepted() {
        // d9d9f7 is CBOR's magic number; it carries no data of its own.
        val decoded = ok(hex("d9d9f7") + hex("83010203"))
        assertEquals(3, (decoded.items.single() as Json.Arr).items.size)
        assertTrue(CborCodec.detect(hex("d9d9f783010203"), ""))
    }

    // --- 5.1.4 damage: located, never fatal, never an allocation bomb -------

    @Test
    fun truncationIsReportedWithAnOffset() {
        // an array of three that stops after two
        val err = CborCodec.decode(hex("830102"))
        assertTrue(err is BinaryDecodeResult.Err)
        assertTrue((err as BinaryDecodeResult.Err).offset >= 0)
    }

    @Test
    fun aLyingLengthPrefixFailsInsteadOfAllocating() {
        // A 4-byte header claiming ~4 billion elements, in a 5-byte file. A
        // decoder that trusts the prefix dies here; this one must not.
        val err = CborCodec.decode(hex("9affffffff"))
        assertTrue("expected a located error, got $err", err is BinaryDecodeResult.Err)
        assertTrue((err as BinaryDecodeResult.Err).message.contains("remain"))

        val strErr = CborCodec.decode(hex("7affffffff"))
        assertTrue(strErr is BinaryDecodeResult.Err)
    }

    @Test
    fun runawayNestingIsBounded() {
        // 600 nested one-element arrays: deeper than the limit, shallower than
        // anything real.
        val bytes = ByteArray(600) { 0x81.toByte() } + byteArrayOf(0x01)
        val err = CborCodec.decode(bytes)
        assertTrue(err is BinaryDecodeResult.Err)
        assertTrue((err as BinaryDecodeResult.Err).message.contains("nesting"))
    }

    @Test
    fun reservedAndBrokenHeadersAreRejected() {
        assertTrue(CborCodec.decode(hex("1c")) is BinaryDecodeResult.Err)   // reserved info 28
        assertTrue(CborCodec.decode(hex("ff")) is BinaryDecodeResult.Err)   // stray break
        assertTrue(CborCodec.decode(ByteArray(0)) is BinaryDecodeResult.Ok) // empty = no items
        assertEquals(0, ok(ByteArray(0)).items.size)
    }

    // --- 5.1.5 registration -------------------------------------------------

    @Test
    fun theRegistryClaimsCborAndLeavesOtherFilesAlone() {
        assertEquals(CborCodec, BinaryJsonCodecs.forFile("fixture.cbor", hex("00")))
        assertEquals(CborCodec, BinaryJsonCodecs.forFile("stream.cborseq", hex("00")))
        // Content beats extension: the magic number identifies it regardless.
        assertEquals(CborCodec, BinaryJsonCodecs.forFile("mystery.bin", hex("d9d9f700")))
        assertNull(BinaryJsonCodecs.forFile("notes.txt", "hello".toByteArray()))
        assertNull(BinaryJsonCodecs.forFile("data.msgpack", hex("82a16101")))
        assertTrue(BinaryJsonCodecs.claimsExtension("CBOR"))
        assertFalse(BinaryJsonCodecs.claimsExtension("json"))
    }

    // --- 5.1.6 tags ---------------------------------------------------------

    @Test
    fun aTagKeepsItsContentAndLocksTheFile() {
        // Tag 1 (epoch time) wrapping 1363896240. The value is what a reader
        // wants to SEE; dropping the tag on save is what must not happen
        // silently, so the file is read-only with the reason.
        val decoded = ok(hex("c11a514b67b0"))
        assertEquals(1363896240.0, (decoded.items.single() as Json.Num).value, 0.0)
        assertFalse(decoded.editable)
        assertTrue(decoded.reason!!.contains("tag 1"))
    }
}
