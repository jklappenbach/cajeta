package dev.cajeta.idea.usages

import dev.cajeta.idea.debugger.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ide-features Unit 2 (spec §2.0.4), pure half: what the index already knows,
 * turned into what the usage view shows. Nothing here searches — the compiler
 * resolved these references, which is the whole reason overloads stay distinct
 * and dependency/stdlib usages are found like any other.
 */
class CajetaUsageRecordsTest {

    private fun rec(json: String): Json.Obj = Json.parse(json) as Json.Obj

    @Test
    fun aReferenceRecordBecomesAUseSite() {
        val site = CajetaUsageRecords.parse(rec(
            """{"file":"src/demo/Prog.cajeta","line":12,"col":8,
                "kind":"call","from":"demo.Prog.main"}"""))!!
        assertEquals("src/demo/Prog.cajeta", site.file)
        assertEquals(12, site.line)
        assertEquals(8, site.col)
        assertEquals(CajetaUsageKind.CALL, site.kind)
        assertEquals("demo.Prog.main", site.from)
    }

    @Test
    fun aRecordWithoutAPositionIsNotAUsage() {
        // The export writes line 0 to mean "no position"; showing that as
        // line zero would put a usage at the top of a file it isn't in.
        assertNull(CajetaUsageRecords.parse(rec("""{"file":"a.cajeta","line":0}""")))
        assertNull(CajetaUsageRecords.parse(rec("""{"file":"","line":3}""")))
        assertNull(CajetaUsageRecords.parse(rec("""{"line":3,"col":1}""")))
        assertNull(CajetaUsageRecords.parse(rec("""{}""")))
    }

    @Test
    fun aMissingColumnIsToleratedAsTheLineStart() {
        val site = CajetaUsageRecords.parse(rec("""{"file":"a.cajeta","line":4}"""))!!
        assertEquals(0, site.col)
        assertEquals(CajetaUsageKind.OTHER, site.kind)
        assertNull(site.from)
    }

    @Test
    fun everyExportedKindMapsToItsGroup() {
        assertEquals(CajetaUsageKind.READ, CajetaUsageKind.of("read"))
        assertEquals(CajetaUsageKind.WRITE, CajetaUsageKind.of("assign"))
        assertEquals(CajetaUsageKind.CALL, CajetaUsageKind.of("invoke"))
        assertEquals(CajetaUsageKind.CALL, CajetaUsageKind.of("new"))
        assertEquals(CajetaUsageKind.INHERIT, CajetaUsageKind.of("implements"))
        assertEquals(CajetaUsageKind.IMPORT, CajetaUsageKind.of("import"))
        assertEquals(CajetaUsageKind.TYPE, CajetaUsageKind.of("typeref"))
        assertEquals(CajetaUsageKind.TYPE, CajetaUsageKind.of("TYPE"))
    }

    @Test
    fun anUnknownKindIsUngroupedNotMislabelled() {
        // A kind the export gains later must show up plainly rather than be
        // quietly filed under whichever group happened to be the fallback.
        assertEquals(CajetaUsageKind.OTHER, CajetaUsageKind.of("something-new"))
        assertEquals(CajetaUsageKind.OTHER, CajetaUsageKind.of(null))
        assertEquals(CajetaUsageKind.OTHER, CajetaUsageKind.of(""))
    }

    @Test
    fun sitesGroupByFileInStableOrder() {
        val sites = CajetaUsageRecords.parseAll(listOf(
            rec("""{"file":"B.cajeta","line":9,"col":2,"kind":"read"}"""),
            rec("""{"file":"A.cajeta","line":7,"col":4,"kind":"call"}"""),
            rec("""{"file":"B.cajeta","line":2,"col":1,"kind":"read"}"""),
            rec("""{"file":"A.cajeta","line":7,"col":1,"kind":"read"}"""),
        ))
        val grouped = CajetaUsageRecords.byFile(sites)

        // File order is first-seen; within a file, position order — so the
        // tree does not reshuffle between two runs of the same search.
        assertEquals(listOf("B.cajeta", "A.cajeta"), grouped.keys.toList())
        assertEquals(listOf(2, 9), grouped["B.cajeta"]!!.map { it.line })
        assertEquals(listOf(1, 4), grouped["A.cajeta"]!!.map { it.col })
    }

    @Test
    fun aClassWithSeveralUsagesKeepsThemAllUnderThatFile() {
        // The "Usages : Class : instance" shape Julian asked for is a file
        // with more than one site under it.
        val sites = CajetaUsageRecords.parseAll(listOf(
            rec("""{"file":"demo/Prog.cajeta","line":3,"kind":"import"}"""),
            rec("""{"file":"demo/Prog.cajeta","line":11,"kind":"type"}"""),
            rec("""{"file":"demo/Prog.cajeta","line":11,"col":30,"kind":"call"}"""),
        ))
        val grouped = CajetaUsageRecords.byFile(sites)
        assertEquals(1, grouped.size)
        assertEquals(3, grouped["demo/Prog.cajeta"]!!.size)
        assertTrue(grouped["demo/Prog.cajeta"]!!.map { it.kind }
            .containsAll(listOf(CajetaUsageKind.IMPORT, CajetaUsageKind.TYPE, CajetaUsageKind.CALL)))
    }

    @Test
    fun malformedRecordsAreSkippedNotFatal() {
        val sites = CajetaUsageRecords.parseAll(listOf(
            rec("""{"file":"ok.cajeta","line":1}"""),
            rec("""{"file":"bad.cajeta"}"""),
            rec("""{"line":"not-a-number"}"""),
        ))
        assertEquals(1, sites.size)
        assertEquals("ok.cajeta", sites.single().file)
    }
}
