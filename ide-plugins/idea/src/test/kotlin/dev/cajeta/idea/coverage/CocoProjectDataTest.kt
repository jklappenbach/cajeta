package dev.cajeta.idea.coverage

import com.intellij.rt.coverage.data.LineCoverage
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ide-coverage-plan Unit 3.1 — the translation from coco's model into the
 * platform's `ProjectData`, which is what actually drives the gutters.
 *
 * Kept free of the IntelliJ test fixture: the mapping is pure, and the traps
 * worth pinning (a null line slot meaning "not instrumented", `fillArrays()`
 * being required before branch status is visible) are all observable here.
 */
class CocoProjectDataTest {

    private fun fixture(name: String): String =
        javaClass.getResourceAsStream("/coco/conformance/$name")
            ?.bufferedReader()?.readText()
            ?: error("missing vendored fixture: $name")

    private fun fixtureCoverage() =
        CocoCoverage(parseCocoSites(fixture("sites.tsv")), parseCocoProfile(fixture("coco.profile")))

    /** Fixture paths are relative to a source root; the annotator wants absolute. */
    private fun convert(c: CocoCoverage = fixtureCoverage()) =
        CocoProjectData.toProjectData(c) { "/src/$it" }

    // --- one ClassData per source file, keyed by path ------------------------

    @Test
    fun eachSourceFileBecomesOneClassDataKeyedByPath() {
        // SimpleCoverageAnnotator keys ProjectData by normalized FILE PATH, not
        // by qualified class name — it builds its lookup from
        // getClasses().keySet() run through normalizeFilePath. Naming these
        // "probe.Cond" would leave every file unannotated.
        val p = convert()
        assertEquals(
            listOf("/src/probe/Cond.cajeta", "/src/probe/Helper.cajeta"),
            p.classes.keys.sorted(),
        )
    }

    @Test
    fun aFileWithNoProbesHasNoClassDataAtAll() {
        // 3.1.f: absent, not present-and-zero. A file coco never instrumented
        // must be left unmarked rather than painted 0%.
        assertNull(convert().getClassData("/src/probe/Untouched.cajeta"))
    }

    // --- 3.1.f  uninstrumented line vs probed-but-unhit line -----------------

    @Test
    fun anUninstrumentedLineIsAbsentNotZero() {
        val cd = convert().getClassData("/src/probe/Helper.cajeta")!!
        // Helper carries probes on lines 5 and 6 only.
        assertTrue("line 5 is instrumented", cd.containsLine(5))
        assertFalse("line 4 has no probe, so it must not be markable", cd.containsLine(4))
        assertFalse("line 7 has no probe", cd.containsLine(7))
        assertNull(cd.getLineData(4))
    }

    @Test
    fun aProbedButUnhitLineIsPresentWithZeroHits() {
        // Line 31 is instrumented and never ran: it must exist and read NONE,
        // which is what paints it red. Confusing this with the uninstrumented
        // case is how coverage tools grow phantom red gutters.
        val cd = convert().getClassData("/src/probe/Cond.cajeta")!!
        val ld = cd.getLineData(31)
        assertNotNull("an unexecuted line is still instrumented", ld)
        assertEquals(0, ld!!.hits)
        assertEquals(LineCoverage.NONE.toInt(), ld.status)
    }

    // --- function probes carry line 0 ---------------------------------------

    @Test
    fun functionProbesNeverBecomeALineDataBecauseTheirLineIsZero() {
        // Every `function` row in coco carries line 0 — NOT a source line, and
        // not what docs/formats.md claimed. Emitting a LineData for it would put
        // a markable entry at index 0 of the line array.
        val cd = convert().getClassData("/src/probe/Cond.cajeta")!!
        assertFalse("no line 0", cd.containsLine(0))
        assertNull(cd.getLineData(0))
    }

    @Test
    fun lineRollupsIgnoreTheZeroLineOfFunctionProbes() {
        val c = fixtureCoverage()
        assertEquals("line 0 is not a source line", 0L, c.lineHits("probe/Cond.cajeta", 0))
        assertFalse(c.instrumentedLines("probe/Cond.cajeta").contains(0))
    }

    // --- 3.1.c  function metric ----------------------------------------------

    @Test
    fun everyMethodIsRegisteredSoFunctionCoverageResolves() {
        val cd = convert().getClassData("/src/probe/Cond.cajeta")!!
        val sigs = cd.methodSigs.toSet()
        assertEquals(
            "all six of Cond's methods, including the uncalled one",
            setOf(
                "probe.Cond.both(b:int32,a:int32)",
                "probe.Cond.either(b:int32,a:int32)",
                "probe.Cond.guarded(n:int32)",
                "probe.Cond.loop(n:int32)",
                "probe.Cond.main()",
                "probe.Cond.neverCalled(n:int32)",
            ),
            sigs,
        )
        assertEquals(
            "main() ran",
            1,
            cd.getStatus("probe.Cond.main()"),
        )
        assertEquals(
            "neverCalled() did not",
            0,
            cd.getStatus("probe.Cond.neverCalled(n:int32)"),
        )
    }

    @Test
    fun aMethodWithOnlyAFunctionProbeIsStillRegistered() {
        // A method coco instrumented at entry but nowhere else would vanish from
        // the function metric if signatures were harvested from line probes only.
        val sites = listOf(
            site(0, CocoSiteKind.FUNCTION, line = 0, method = "onlyEntry()"),
            site(1, CocoSiteKind.LINE, line = 3, method = "other()"),
        )
        val p = CocoProjectData.toProjectData(
            CocoCoverage(sites, CocoProfile(2, null, mapOf(1L to 1L)))
        ) { "/src/$it" }
        val cd = p.getClassData("/src/A.cajeta")!!
        assertTrue(cd.methodSigs.contains("p.A.onlyEntry()"))
        assertEquals("never entered", 0, cd.getStatus("p.A.onlyEntry()"))
    }

    // --- 3.1.c  line metric, max not sum -------------------------------------

    @Test
    fun lineHitsCarryTheMaximumThroughTheConversion() {
        val sites = listOf(
            site(0, CocoSiteKind.LINE, line = 7),
            site(1, CocoSiteKind.LINE, line = 7),
            site(2, CocoSiteKind.LINE, line = 7),
        )
        val p = CocoProjectData.toProjectData(
            CocoCoverage(sites, CocoProfile(3, null, mapOf(0L to 4L, 1L to 4L, 2L to 4L)))
        ) { "/src/$it" }
        // Summing would report a line run four times as run twelve.
        assertEquals(4, p.getClassData("/src/A.cajeta")!!.getLineData(7)!!.hits)
    }

    @Test
    fun hitCountsAreClampedRatherThanOverflowingInt() {
        // coco counts in 64 bits; LineData.setHits takes an int and then applies
        // the platform's own ceiling of 1_000_000_000. What must not happen is a
        // wrap into the negatives: trimHits maps anything negative to that SAME
        // ceiling, so an overflowed count would come back as "ran a billion
        // times" rather than as an obvious error.
        val sites = listOf(site(0, CocoSiteKind.LINE, line = 2))
        val p = CocoProjectData.toProjectData(
            CocoCoverage(sites, CocoProfile(1, null, mapOf(0L to 5_000_000_000L)))
        ) { "/src/$it" }
        val hits = p.getClassData("/src/A.cajeta")!!.getLineData(2)!!.hits
        assertTrue("saturated, not wrapped: $hits", hits > 0)
        assertEquals(PLATFORM_MAX_HITS, hits)
    }

    // --- 3.1.c  branch metric -------------------------------------------------

    @Test
    fun aPartiallyTakenBranchReportsPartial() {
        // The fillArrays() trap: JumpsAndSwitches keeps jumps in a List until
        // fillArrays() moves them into the array that getStatus() reads. Skip it
        // and every partial branch silently reports FULL — green gutters on a
        // decision that only ever went one way.
        val p = branchFixture(trueHits = 5L, falseHits = 0L, lineHits = 5L)
        val ld = p.getClassData("/src/A.cajeta")!!.getLineData(3)!!
        assertEquals(1, ld.jumpsCount())
        assertEquals(5, ld.jumps!![0].trueHits)
        assertEquals(0, ld.jumps!![0].falseHits)
        assertEquals(LineCoverage.PARTIAL.toInt(), ld.status)
    }

    @Test
    fun aFullyTakenBranchReportsFull() {
        val p = branchFixture(trueHits = 5L, falseHits = 2L, lineHits = 7L)
        assertEquals(
            LineCoverage.FULL.toInt(),
            p.getClassData("/src/A.cajeta")!!.getLineData(3)!!.status,
        )
    }

    @Test
    fun anUnreachedDecisionLeavesItsLineUncovered() {
        val p = branchFixture(trueHits = 0L, falseHits = 0L, lineHits = 0L)
        val ld = p.getClassData("/src/A.cajeta")!!.getLineData(3)!!
        assertEquals(LineCoverage.NONE.toInt(), ld.status)
        assertEquals(0, ld.jumps!![0].trueHits)
        assertEquals(0, ld.jumps!![0].falseHits)
    }

    @Test
    fun theFixturesEightDecisionsAllSurviveAsJumpsAndNoneAreInvented() {
        // addJump(n) PADS the line's jump list out to n+1 entries, so an index
        // that runs across the file instead of restarting per line manufactures
        // empty jumps — which read as unevaluated decisions and quietly drag
        // every branch percentage down. 8 in, 8 out.
        val p = convert()
        var jumps = 0
        for (name in p.classes.keys) {
            val cd = p.getClassData(name)!!
            for (o in cd.lines) {
                val ld = o as? com.intellij.rt.coverage.data.LineData ?: continue
                jumps += ld.jumpsCount()
            }
        }
        assertEquals("8 decisions in, 8 jumps out", 8, jumps)
    }

    @Test
    fun twoDecisionsOnOneLineBothSurvive() {
        // `a && b` puts two decisions on one line; they must become two jumps on
        // that line, not one, and not three.
        val cd = convert().getClassData("/src/probe/Cond.cajeta")!!
        assertEquals("line 5 carries a && b", 2, cd.getLineData(5)!!.jumpsCount())
        assertEquals("line 19 carries one decision", 1, cd.getLineData(19)!!.jumpsCount())
    }

    @Test
    fun theFixturesPerFileTotalsMatchTheArtifact() {
        val cd = convert().getClassData("/src/probe/Cond.cajeta")!!
        var totalLines = 0
        var coveredLines = 0
        var arms = 0
        var takenArms = 0
        for (o in cd.lines) {
            val ld = o as? com.intellij.rt.coverage.data.LineData ?: continue
            totalLines++
            if (ld.hits > 0) coveredLines++
            for (j in ld.jumps ?: emptyArray()) {
                arms += 2
                if (j.trueHits > 0) takenArms++
                if (j.falseHits > 0) takenArms++
            }
        }
        assertEquals(21, totalLines)
        assertEquals(14, coveredLines)
        assertEquals(14, arms)
        assertEquals(7, takenArms)
        assertEquals(4, cd.methodSigs.count { (cd.getStatus(it) ?: 0) != 0 })
    }

    // --- the source path is recorded so the view can find the file -----------

    @Test
    fun classDataCarriesItsSourceFileName() {
        assertEquals(
            "Cond.cajeta",
            convert().getClassData("/src/probe/Cond.cajeta")!!.source,
        )
    }

    private fun branchFixture(trueHits: Long, falseHits: Long, lineHits: Long) =
        CocoProjectData.toProjectData(
            CocoCoverage(
                listOf(
                    site(0, CocoSiteKind.LINE, line = 3),
                    site(1, CocoSiteKind.BRANCH_TRUE, line = 3, block = "b1"),
                    site(2, CocoSiteKind.BRANCH_FALSE, line = 3, block = "b1"),
                ),
                CocoProfile(
                    size = 3,
                    label = null,
                    hits = buildMap {
                        if (lineHits > 0) put(0L, lineHits)
                        if (trueHits > 0) put(1L, trueHits)
                        if (falseHits > 0) put(2L, falseHits)
                    },
                ),
            )
        ) { "/src/$it" }

    private companion object {
        /** `ClassData.trimHits` caps every count here, wrapped or not. */
        const val PLATFORM_MAX_HITS = 1_000_000_000
    }

    private fun site(
        id: Long,
        kind: CocoSiteKind,
        line: Int,
        block: String = "",
        method: String = "m()",
    ) = CocoSite(
        id = id, kind = kind, line = line, decision = -1L,
        file = "A.cajeta", owner = "p.A", method = method, block = block, target = "",
    )
}
