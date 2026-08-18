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
    fun anUncalledMethodIsADeletionCandidate() {
        val n = CocoDeadCode.classify(coverage(), realisticEdges())
            .single { it.method.startsWith("neverCalled") }
        assertEquals(Verdict.DELETION_CANDIDATE, n.verdict)
        assertTrue("explains the absence of any path: ${n.reason}", n.reason.contains("no call path"))
    }

    @Test
    fun theTwoAreClassifiedDIFFERENTLYOnTheSameRun() {
        // 6.3.b in miniature, and the whole point of the unit: an lcov report
        // shows these as identical red.
        val out = CocoDeadCode.classify(coverage(), realisticEdges())
        assertEquals(2, out.map { it.verdict }.toSet().size)
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

    // --- 6.1.d  navigation needs a real target --------------------------------

    @Test
    fun eachClassificationCarriesAFileAndALineToNavigateTo() {
        for (m in CocoDeadCode.classify(coverage(), realisticEdges())) {
            assertNotNull(m.file)
            assertTrue("${m.displayName} has a real source line, not the function probe's 0", m.line > 0)
        }
    }
}
