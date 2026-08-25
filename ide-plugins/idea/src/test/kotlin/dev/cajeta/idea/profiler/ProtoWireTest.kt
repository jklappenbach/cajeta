package dev.cajeta.idea.profiler

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * cajeta-profiler Unit 11.2.a — the protobuf wire reader.
 *
 * The viewer decodes Perfetto traces by hand rather than through generated
 * classes: the IntelliJ platform ships a JetBrains fork of protobuf-java on the
 * platform classpath, and this plugin declares 242 -> 299.*, so generated code
 * would pin the viewer to one line of a runtime nobody here controls. The
 * writer hand-encodes for the same reason (profiler spec §7).
 *
 * What that buys has to be paid for here. Vectors are taken from protobuf's own
 * documented encodings — https://protobuf.dev/programming-guides/encoding/ —
 * so the decoder is checked against the format rather than against itself. A
 * decoder tested only with bytes its matching encoder produced agrees with its
 * own mistakes.
 */
class ProtoWireTest {

    private fun bytes(vararg v: Int): ByteArray = v.map { it.toByte() }.toByteArray()

    private fun reader(vararg v: Int): ProtoWire = ProtoWire(bytes(*v))

    // --- varints, from the encoding guide's own examples ------------------

    @Test
    fun singleByteVarints() {
        assertEquals(0L, reader(0x00).readVarint())
        assertEquals(1L, reader(0x01).readVarint())
        assertEquals(127L, reader(0x7f).readVarint())
    }

    @Test
    fun multiByteVarintsAreLittleEndianGroupsOfSeven() {
        // 150 = 0x96 0x01 — the guide's worked example.
        assertEquals(150L, reader(0x96, 0x01).readVarint())
        // 300 = 0xAC 0x02, and 270 = 0x8E 0x02, which differ only in the low
        // group: a decoder that shifted the wrong way would swap them.
        assertEquals(300L, reader(0xAC, 0x02).readVarint())
        assertEquals(270L, reader(0x8E, 0x02).readVarint())
        assertEquals(86942L, reader(0x9E, 0xA7, 0x05).readVarint())
    }

    @Test
    fun aFullWidthVarintDoesNotOverflowIntoTheSignBit() {
        // 2^64-1 is ten bytes. Kotlin's Long is signed, so this comes back as
        // -1; what matters is that the bits survive, since track uuids and flow
        // ids routinely use the high bit and a decoder that saturated or threw
        // would lose exactly the ones the profiler generates (uuid tags are
        // built by shifting a marker into the top bits).
        val r = ProtoWire(bytes(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01))
        assertEquals(-1L, r.readVarint())
        assertFalse(r.hasMore())
    }

    // --- tags: field number and wire type share one varint ----------------

    @Test
    fun aTagCarriesFieldNumberAndWireType() {
        // 0x08 = field 1, wire type 0 (the guide's example).
        val r = reader(0x08, 0x96, 0x01)
        val tag = r.readTag()
        assertEquals(1, ProtoWire.fieldOf(tag))
        assertEquals(ProtoWire.VARINT, ProtoWire.wireOf(tag))
        assertEquals(150L, r.readVarint())

        // Field 60 (TrackDescriptor) is two bytes of tag — the profiler's own
        // fields run well past the single-byte range, which is where a decoder
        // that assumed one-byte tags stops working.
        val t2 = ProtoWire(bytes(0xE2, 0x03, 0x00)).readTag()
        assertEquals(60, ProtoWire.fieldOf(t2))
        assertEquals(ProtoWire.LEN, ProtoWire.wireOf(t2))
    }

    // --- length-delimited -------------------------------------------------

    @Test
    fun lengthDelimitedFieldsCarryTheirOwnLength() {
        // Field 2, "testing" — the guide's string example.
        val r = ProtoWire(bytes(0x12, 0x07, 0x74, 0x65, 0x73, 0x74, 0x69, 0x6e, 0x67))
        val tag = r.readTag()
        assertEquals(2, ProtoWire.fieldOf(tag))
        assertEquals(ProtoWire.LEN, ProtoWire.wireOf(tag))
        assertEquals("testing", r.readString())
        assertFalse(r.hasMore())
    }

    @Test
    fun anEmptyLengthDelimitedFieldIsNotAnError() {
        val r = ProtoWire(bytes(0x12, 0x00))
        r.readTag()
        assertArrayEquals(ByteArray(0), r.readBytes())
    }

    // --- fixed64: the one place a varint would silently be wrong ----------

    @Test
    fun fixed64IsEightLittleEndianBytesNotAVarint() {
        // TrackEvent.flow_ids (47) and terminating_flow_ids (48) are the only
        // fixed64 fields the writer emits, and their deprecated varint twins
        // (36, 42) still exist in the schema — so a reader that guessed varint
        // would parse cleanly and silently find no flows at all.
        val r = ProtoWire(bytes(0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00))
        assertEquals(1L, r.readFixed64())

        val r2 = ProtoWire(bytes(0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01))
        assertEquals(0x0123456789ABCDEFL, r2.readFixed64())
    }

    // --- skipping: the whole reason a viewer survives a schema it predates --

    @Test
    fun anUnknownFieldOfEveryWireTypeCanBeSkipped() {
        // A trace from a newer writer will carry fields this build has never
        // heard of. Skipping by wire type is what lets the reader keep going
        // instead of failing on a file that is perfectly valid.
        val r = ProtoWire(bytes(
            0x08, 0x96, 0x01,                      // field 1, varint 150
            0x11, 1, 0, 0, 0, 0, 0, 0, 0,          // field 2, fixed64
            0x1A, 0x02, 0x41, 0x42,                // field 3, len "AB"
            0x25, 4, 3, 2, 1,                      // field 4, fixed32
            0x28, 0x07,                            // field 5, varint 7
        ))
        var last = -1L
        while (r.hasMore()) {
            val tag = r.readTag()
            if (ProtoWire.fieldOf(tag) == 5) { last = r.readVarint(); break }
            r.skip(ProtoWire.wireOf(tag))
        }
        assertEquals(7L, last)
        assertFalse(r.hasMore())
    }

    @Test
    fun aTruncatedFieldIsReportedRatherThanReadPastTheEnd() {
        // Traces are written by a process that can be killed mid-write, so a
        // truncated file is an ordinary thing to open, not a corruption to
        // crash on.
        val r = ProtoWire(bytes(0x12, 0x40, 0x41))   // claims 64 bytes, has 1
        r.readTag()
        var threw = false
        try { r.readBytes() } catch (e: ProtoWire.Truncated) { threw = true }
        assertTrue("a truncated length-delimited field must be reported", threw)
    }

    @Test
    fun aSubMessageIsReadWithinItsOwnBounds() {
        // Nested messages are the shape of every packet: a reader that ran past
        // a sub-message's end would read its parent's next field as its own.
        val r = ProtoWire(bytes(
            0x0A, 0x02, 0x08, 0x2A,   // field 1, len 2: { field 1 varint 42 }
            0x10, 0x63,               // field 2, varint 99  (the PARENT's)
        ))
        r.readTag()
        val inner = r.readSub()
        assertEquals(1, ProtoWire.fieldOf(inner.readTag()))
        assertEquals(42L, inner.readVarint())
        assertFalse("the sub-reader ran past its own end", inner.hasMore())

        assertEquals(2, ProtoWire.fieldOf(r.readTag()))
        assertEquals(99L, r.readVarint())
    }
}
