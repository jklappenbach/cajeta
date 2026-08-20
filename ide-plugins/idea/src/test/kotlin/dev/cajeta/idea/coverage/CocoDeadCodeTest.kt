package dev.cajeta.idea.coverage

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ide-coverage-plan Unit 6.1.a–c — classifying uncovered code against the
 * conformance fixture, which conveniently contains both shapes: a method that is
 * called but never exercised, and one that nothing calls at all.
 */
class CocoDeadCodeTest {

    private fun fixture(name: String) =
        javaClass.getResourceAsStream("/coco/conformance/$name")!!.bufferedReader().readText()

    private fun coverage() =
        CocoCoverage(parseCocoSites(fixture("sites.tsv")), parseCocoProfile(fixture("coco.profile")))

    private class Edges(
        val calls: Map<String, List<String>> = emptyMap(),
        val known: Set<String> = emptySet(),
    ) : XrefEdges {
        override fun callersOf(key: String) = calls[key].orEmpty()
        override fun basesOf(key: String) = emptyList<String>()
        override fun isKnown(key: String) = key in known
    }

    /** guarded() is called by main(); neverCalled() is called by nobody. */
    private fun realisticEdges() = Edges(
        calls = mapOf(
            "probe.Cond::guarded/1" to listOf("probe.Cond::main/0"),
            "probe.Cond::neverCalled/1" to emptyList(),
        ),
        known = setOf(
            "probe.Cond::guarded/1", "probe.Cond::neverCalled/1", "probe.Cond::main/0",
        ),
    )

    // --- 6.1.a / 6.1.b / 6.1.c ------------------------------------------------

    @Test
    fun onlyUncoveredMethodsAreClassified() {
        val out = CocoDeadCode.classify(coverage(), realisticEdges())
        assertEquals(
            "the fixture's two unexecuted methods, and no others",
            listOf("probe.Cond.guarded(n:int32)", "probe.Cond.neverCalled(n:int32)"),
            out.map { it.displayName }.sorted(),
        )
    }

    @Test
    fun aReachableButUnexecutedMethodNeedsATestNotDeleting() {
        // guarded() is called from main(); main() ran. The call just never took
        // the branch that enters it. Proposing deletion here would be wrong.
        val g = CocoDeadCode.classify(coverage(), realisticEdges())
            .single { it.method.startsWith("guarded") }
        assertEquals(Verdict.NEEDS_A_TEST, g.verdict)
        assertTrue("says a test is the fix: ${g.reason}", g.reason.contains("test"))
    }

    @Test
    fun anUncalledMethodOnALiveClassIsSparedAndSaysWhy() {
        // This test used to assert DELETION_CANDIDATE. That was wrong, and
        // diffing against a real coco run is what proved it (6.1.e): nothing on
        // a class with executed methods may be called dead. The verdict is
        // spared, and the reason states the actual basis rather than implying a
        // call path was found.
        val n = CocoDeadCode.classify(coverage(), realisticEdges())
            .single { it.method.startsWith("neverCalled") }
        assertEquals(Verdict.NEEDS_A_TEST, n.verdict)
        assertTrue(
            "names the liveness basis, not a fictitious call path: ${n.reason}",
            n.reason.contains("class has executed methods"),
        )
        assertTrue("and names why that matters: ${n.reason}", n.reason.contains("reflection"))
    }

    @Test
    fun theTwoVerdictsAreDistinguishableOnOneRun() {
        // 6.3.b in miniature, and the whole point of the unit: an lcov report
        // shows these as identical red. Needs a genuinely dead CLASS, since a
        // dead method on a live class is spared by the liveness guard — which
        // is itself the finding that makes this test's shape non-obvious.
        val sites = coverage().sites + listOf(
            CocoSite(900, CocoSiteKind.FUNCTION, 0, -1L, "Z.cajeta", "p.Zombie", "gone()", "", ""),
            CocoSite(901, CocoSiteKind.LINE, 4, -1L, "Z.cajeta", "p.Zombie", "gone()", "", ""),
        )
        val edges = Edges(
            calls = realisticEdges().calls + mapOf("p.Zombie::gone/0" to emptyList()),
            known = realisticEdges().known + setOf("p.Zombie::gone/0"),
        )
        val out = CocoDeadCode.classify(
            CocoCoverage(sites, coverage().profile), edges,
        )
        val verdicts = out.map { it.verdict }.toSet()
        assertTrue("both kinds of finding present: $verdicts", verdicts.size >= 2)
        assertEquals(
            Verdict.DELETION_CANDIDATE,
            out.single { it.owner == "p.Zombie" }.verdict,
        )
        assertEquals(
            Verdict.NEEDS_A_TEST,
            out.single { it.method.startsWith("guarded") }.verdict,
        )
    }

    // --- 6.1.c  undeterminable is stated, never defaulted --------------------

    @Test
    fun aStaleIndexMakesEverythingUndeterminedRatherThanDead() {
        // Without this, an index that has not caught up would present the whole
        // codebase as deletable.
        val out = CocoDeadCode.classify(
            coverage(), realisticEdges(), indexReason = "the Cajeta index is not up to date",
        )
        assertTrue(out.isNotEmpty())
        assertTrue("nothing is called dead", out.all { it.verdict == Verdict.UNDETERMINED })
        assertTrue(out.all { it.reason.contains("not up to date") })
    }

    @Test
    fun aMethodAbsentFromTheIndexIsUndeterminedNotDead() {
        val out = CocoDeadCode.classify(coverage(), Edges(known = setOf("probe.Cond::main/0")))
        assertTrue(
            "absence of evidence is not evidence of death",
            out.all { it.verdict == Verdict.UNDETERMINED },
        )
    }

    @Test
    fun coverageSeededRootsKeepReflectivelyInvokedCodeOffTheDeletionList() {
        // A method with NO static caller that the profile shows ran is a root.
        // Its callees are then reachable, even though a purely static graph
        // would call the whole cluster dead.
        val edges = Edges(
            calls = mapOf("probe.Cond::neverCalled/1" to listOf("probe.Cond::loop/1")),
            known = setOf("probe.Cond::neverCalled/1", "probe.Cond::loop/1"),
        )
        // loop() executed in the fixture, so it seeds as a root.
        val n = CocoDeadCode.classify(coverage(), edges).single { it.method.startsWith("neverCalled") }
        assertEquals(Verdict.NEEDS_A_TEST, n.verdict)
    }

    // --- 6.1.e  the class-liveness guard, from coco's own policy -------------

    @Test
    fun noMemberOfALiveClassIsEverCalledDead() {
        // coco requires TWO conditions for dead: statically unreachable AND the
        // owning class had no method execute. Its own words: "A 'dead' claim on
        // any member of a class with executed methods would be wrong with
        // confidence — this is the guard that prevents it." Compile-time DI and
        // reflection reach members through synthesized paths a static graph
        // cannot see, so a live class is evidence its members may be reachable.
        //
        // This was found by running the real coco against the same project and
        // diffing: coco said "untested", this said "delete". coco was right.
        val out = CocoDeadCode.classify(coverage(), realisticEdges())
        val n = out.single { it.method.startsWith("neverCalled") }
        assertEquals(
            "probe.Cond has executed methods, so nothing in it may be called dead",
            Verdict.NEEDS_A_TEST,
            n.verdict,
        )
    }

    @Test
    fun aMethodOnAWhollyDeadClassIsStillADeletionCandidate() {
        // The guard must not swallow the capability: when NOTHING in the class
        // ran, the static verdict stands and the deletion candidate survives.
        val sites = listOf(
            CocoSite(0, CocoSiteKind.FUNCTION, 0, -1L, "Z.cajeta", "p.Zombie", "gone()", "", ""),
            CocoSite(1, CocoSiteKind.LINE, 4, -1L, "Z.cajeta", "p.Zombie", "gone()", "", ""),
        )
        val edges = Edges(
            calls = mapOf("p.Zombie::gone/0" to emptyList()),
            known = setOf("p.Zombie::gone/0", "p.Live::main/0"),
        )
        val out = CocoDeadCode.classify(
            CocoCoverage(sites, CocoProfile(2, null, emptyMap())),
            edges,
            entryKeys = setOf("p.Live::main/0"),
        )
        assertEquals(Verdict.DELETION_CANDIDATE, out.single().verdict)
    }

    // --- 6.1.d  navigation needs a real target --------------------------------

    @Test
    fun eachClassificationCarriesAFileAndALineToNavigateTo() {
        for (m in CocoDeadCode.classify(coverage(), realisticEdges())) {
            assertNotNull(m.file)
            assertTrue("${m.displayName} has a real source line, not the function probe's 0", m.line > 0)
        }
    }
}
