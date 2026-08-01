package dev.cajeta.idea.buildtool

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * compiler-jsonl Unit 2 — version negotiation.
 *
 * The stream is a public interface the moment the IDE depends on it, so the
 * reader has to answer three questions the same way every time: a major it
 * cannot read is REFUSED wholesale (a half-understood stream is the
 * wrong-answer machine this design exists to prevent, spec 2.1.4), an unknown
 * `kind` is SKIPPED so adding a record type stays a minor bump (2.1.5), and an
 * unknown FIELD is ignored for the same reason (2.1.6).
 *
 * The fourth question is reverse compatibility: a stream from a compiler older
 * than the envelope carries no `stream` record at all, and must still parse
 * (spec 6.2.2). Pure / off-platform.
 */
class CompilerStreamReaderTest {

    private val streamRec =
        """{"kind":"stream","major":1,"minor":0,"producer":"cajeta 0.10.0"}"""
    private val diagRec =
        """{"kind":"diagnostic","severity":"error","code":"CJ1","message":"boom","file":null,"line":null,"column":null}"""
    private val progressRec =
        """{"kind":"progress","phase":"parse","state":"start","label":"Parsing"}"""

    @Test
    fun readsASupportedStreamWithItsVersionAndRecords() {
        val s = CompilerStreamReader.read("$streamRec\n$progressRec\n$diagRec\n")
        assertTrue(s.supported)
        assertFalse(s.legacy)
        assertEquals(1, s.versionMajor)
        assertEquals(0, s.versionMinor)
        assertEquals("cajeta 0.10.0", s.producer)
        assertEquals(listOf("progress", "diagnostic"), s.records.map { it.kind })
    }

    // 2.1.1 — an unreadable major is refused WHOLESALE. Not "parse what we
    // recognise": the point of a major bump is that recognition is unreliable.
    @Test
    fun refusesAnUnknownMajorWholesale() {
        val future = """{"kind":"stream","major":99,"minor":0,"producer":"cajeta 9.9.9"}"""
        val s = CompilerStreamReader.read("$future\n$progressRec\n$diagRec\n")
        assertFalse(s.supported)
        assertEquals(99, s.versionMajor)
        assertTrue("a refused stream must yield no records", s.records.isEmpty())
    }

    // 2.3.1 — the verdict is ONE property of the stream, not a per-record flag,
    // so a consumer physically cannot report it once per line.
    @Test
    fun refusalIsASingleVerdictNotPerRecord() {
        val future = """{"kind":"stream","major":99,"minor":0}"""
        val many = (1..25).joinToString("\n") { diagRec }
        val s = CompilerStreamReader.read("$future\n$many\n")
        assertFalse(s.supported)
        assertEquals(0, s.records.size)
    }

    // 2.1.2 — an unknown kind is data we don't understand yet, not a broken
    // stream. The records AROUND it must still arrive.
    @Test
    fun unknownKindIsSkippedAndParsingContinues() {
        val future = """{"kind":"somethingNew","payload":{"a":1}}"""
        val s = CompilerStreamReader.read("$streamRec\n$progressRec\n$future\n$diagRec\n")
        assertTrue(s.supported)
        assertEquals(listOf("progress", "diagnostic"), s.known.map { it.kind })
        assertEquals(listOf("somethingNew"), s.unknown.map { it.kind })
        // Still carried, so a consumer that learns the kind later can use it.
        assertEquals(3, s.records.size)
    }

    // 2.1.6 — adding a field is a minor bump; an old reader ignores it.
    @Test
    fun unknownFieldOnAKnownKindIsIgnored() {
        val fat =
            """{"kind":"diagnostic","severity":"warning","message":"m","futureField":{"a":1}}"""
        val s = CompilerStreamReader.read("$streamRec\n$fat\n")
        assertTrue(s.supported)
        assertEquals(1, s.records.size)
        assertEquals("diagnostic", s.records[0].kind)
        assertEquals("warning", s.records[0].str("severity"))
    }

    // 6.2.2 — a compiler older than the envelope emits no `stream` record and
    // no `kind` on its diagnostics. A new plugin must still read it.
    @Test
    fun legacyStreamWithoutAStreamRecordStillParses() {
        val legacyDiag =
            """{"severity":"error","code":"CJ1","message":"boom","file":null,"line":null,"column":null}"""
        val s = CompilerStreamReader.read("$legacyDiag\n")
        assertTrue("a pre-envelope stream must not be refused", s.supported)
        assertTrue(s.legacy)
        assertEquals(null, s.versionMajor)
        assertEquals(listOf("diagnostic"), s.records.map { it.kind })
    }

    // Non-JSON console noise is not a stream error — the compiler's stderr is a
    // shared channel and always has been.
    @Test
    fun nonJsonLinesAreIgnored() {
        val s = CompilerStreamReader.read(
            "$streamRec\nplain console text\n\n$diagRec\nnot json either\n")
        assertTrue(s.supported)
        assertEquals(listOf("diagnostic"), s.records.map { it.kind })
    }
}
