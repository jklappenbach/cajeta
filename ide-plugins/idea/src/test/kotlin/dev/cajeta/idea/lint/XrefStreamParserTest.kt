package dev.cajeta.idea.lint

import dev.cajeta.idea.buildtool.JsonDiagnosticParser
import dev.cajeta.idea.debugger.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ide-symbol-index Unit 3 (plan 3.1.2, 3.1.5): the lint stderr is ONE stream
 * carrying two record kinds — diagnostics and xref — and each parser takes only
 * its own, tolerantly. The version handshake refuses an unknown major wholesale:
 * a half-understood index misleads with total confidence, so the choice is all
 * or nothing. Pure / off-platform.
 */
class XrefStreamParserTest {

    private val version = """{"kind":"xref","rel":"version","record":{"major": 1, "minor": 0}}"""
    private val decl =
        """{"kind":"xref","rel":"declarations","record":{"fqn": "demo.Target", "kind": "class", "file": "demo/Target.cajeta", "line": 2, "col": 13}}"""
    private val ref =
        """{"kind":"xref","rel":"references","record":{"target": "demo.Helper", "kind": "type", "file": "demo/Target.cajeta", "line": 3, "col": 4}}"""
    private val diagnostic =
        """{"severity":"error","code":"CJ1","message":"bad","file":"/p/T.cajeta","line":9,"column":2}"""

    // ---- 3.1.2 — demultiplexing a mixed stream --------------------------------

    @Test
    fun mixedStreamSplitsCleanlyBetweenTheTwoParsers() {
        val stderr = listOf(diagnostic, version, decl, ref, "plain console noise")
            .joinToString("\n")

        // The xref side sees exactly the xref records.
        val xref = XrefStreamParser.demux(stderr)
        assertTrue(xref.supported)
        assertEquals(1, xref.versionMajor)
        assertEquals(listOf("declarations", "references"), xref.records.map { it.rel })
        val fqn = (xref.records[0].record.opt("fqn") as Json.Str).value
        assertEquals("demo.Target", fqn)

        // The diagnostic side sees exactly the diagnostic — the xref lines have
        // no `severity`, so the existing parser never even notices them.
        val diags = stderr.lineSequence().mapNotNull { JsonDiagnosticParser.parse(it) }.toList()
        assertEquals(1, diags.size)
        assertEquals("CJ1", diags[0].code)
    }

    @Test
    fun unknownRecordKindIsSkippedNotFatal() {
        // A future record kind on the shared channel is not ours and not an error.
        val stderr = listOf(
            """{"kind":"metrics","rel":"declarations","record":{"fqn": "x"}}""",
            """{"kind":"progress","phase":"parse","state":"start","label":null}""",
            version,
            decl,
        ).joinToString("\n")

        val xref = XrefStreamParser.demux(stderr)
        assertEquals(1, xref.records.size)
        assertEquals("declarations", xref.records[0].rel)
    }

    @Test
    fun unknownRelWithinXrefIsCarriedNotDropped() {
        // A minor schema bump may add relations. The parser carries them; the
        // consumer ignores what it does not know at USE time. Dropping here would
        // turn a compatible minor bump into silent data loss.
        val stderr = listOf(
            version,
            """{"kind":"xref","rel":"macros","record":{"fqn": "demo.M"}}""",
            decl,
        ).joinToString("\n")

        val xref = XrefStreamParser.demux(stderr)
        assertEquals(listOf("macros", "declarations"), xref.records.map { it.rel })
    }

    @Test
    fun malformedLinesAreSkippedNotFatal() {
        val stderr = listOf(
            "{ not json",
            """{"kind":"xref"}""",                          // no rel, no record
            """{"kind":"xref","rel":"declarations"}""",     // no record
            """{"kind":"xref","rel":"declarations","record":"not an object"}""",
            version,
            decl,
        ).joinToString("\n")

        val xref = XrefStreamParser.demux(stderr)
        assertEquals(1, xref.records.size)
    }

    // ---- 3.1.5 — the version handshake -----------------------------------------

    @Test
    fun unknownMajorRefusesAllRecordsRatherThanPartiallyReading() {
        val v2 = """{"kind":"xref","rel":"version","record":{"major": 2, "minor": 0}}"""
        val xref = XrefStreamParser.demux(listOf(v2, decl, ref).joinToString("\n"))

        assertFalse(xref.supported)
        assertEquals(2, xref.versionMajor)
        assertTrue("records from an unknown major must be refused wholesale",
            xref.records.isEmpty())
    }

    @Test
    fun recordsWithNoVersionLineAreRefused() {
        // Nothing vouches for their shape; refusing beats guessing (§2.0.6).
        val xref = XrefStreamParser.demux(listOf(decl, ref).joinToString("\n"))
        assertFalse(xref.supported)
        assertNull(xref.versionMajor)
        assertTrue(xref.records.isEmpty())
    }

    @Test
    fun anEmptyOrDiagnosticsOnlyStreamIsSupportedAndEmpty() {
        // No records = nothing to refuse: the flag stays quiet so callers do not
        // warn on every ordinary lint run.
        assertTrue(XrefStreamParser.demux("").supported)
        val xref = XrefStreamParser.demux(diagnostic)
        assertTrue(xref.supported)
        assertTrue(xref.records.isEmpty())
    }

    @Test
    fun brokenBufferStreamIsVersionOnlyAndSupported() {
        // A syntax-broken buffer yields a version line and no records (2.0.5).
        // That is the KEEP-YOUR-PREVIOUS-INDEX signal, not an error.
        val xref = XrefStreamParser.demux(version)
        assertTrue(xref.supported)
        assertEquals(1, xref.versionMajor)
        assertTrue(xref.records.isEmpty())
    }

    // ---- argv: the xref flag rides the existing invocation ---------------------

    @Test
    fun lintArgvAppendsBareEmitXrefOnlyWhenAsked() {
        val without = CajetacRunner.lintArgv("/bin/cajeta", "/tmp/f.cajeta", "/root", "/root/f.cajeta")
        assertFalse(without.contains("--emit-xref"))

        val with = CajetacRunner.lintArgv("/bin/cajeta", "/tmp/f.cajeta", "/root", "/root/f.cajeta",
            emitXref = true)
        assertEquals(
            listOf("/bin/cajeta", "--lint", "/tmp/f.cajeta", "--diag-format=json",
                   "--source-root", "/root", "--shadow", "/root/f.cajeta", "--emit-xref"),
            with)
    }
}
