package dev.cajeta.idea.coverage

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * ide-coverage-plan Unit 6.1 — coco's reachability POLICY, mirrored.
 *
 * coco documents three rules, and 6.1.e requires this to agree with its HTML
 * report on the same inputs, so all three are reproduced deliberately:
 *
 *  1. **Edges**: calls, plus overrides as base→impl — the conservative
 *     virtual-dispatch approximation, where a reachable base reaches every
 *     override.
 *  2. **Roots**: the entry point, PLUS every method the profile shows executed —
 *     coverage-seeded roots, because reflection and DI are invisible to a static
 *     graph and executed code is ground truth.
 *  3. **Error direction**: degrade toward "untested", so the analysis can miss
 *     dead code but can NEVER call a live method dead.
 *
 * Rule 3 is the one that matters most: the output of this analysis is a
 * suggestion to delete code.
 */
class CocoReachabilityTest {

    /** A hand-built edge source, so the policy is testable without an index. */
    private class Edges(
        val calls: Map<String, List<String>> = emptyMap(),
        val bases: Map<String, List<String>> = emptyMap(),
        val known: Set<String> = emptySet(),
    ) : XrefEdges {
        override fun callersOf(key: String) = calls[key].orEmpty()
        override fun basesOf(key: String) = bases[key].orEmpty()
        override fun isKnown(key: String) = key in known
    }

    // --- rule 1: call edges ---------------------------------------------------

    @Test
    fun aMethodCalledFromTheEntryPointIsReachable() {
        val r = CocoReachability(
            Edges(calls = mapOf("a.C::leaf/0" to listOf("a.C::mid/0"), "a.C::mid/0" to listOf("a.C::main/0")),
                  known = setOf("a.C::leaf/0", "a.C::mid/0", "a.C::main/0")),
            roots = setOf("a.C::main/0"),
        )
        assertEquals(Reachability.REACHABLE, r.of("a.C::leaf/0"))
    }

    @Test
    fun aMethodWithNoCallerChainToARootIsUnreachable() {
        val r = CocoReachability(
            Edges(calls = mapOf("a.C::orphan/0" to emptyList()),
                  known = setOf("a.C::orphan/0", "a.C::main/0")),
            roots = setOf("a.C::main/0"),
        )
        assertEquals(Reachability.UNREACHABLE, r.of("a.C::orphan/0"))
    }

    // --- rule 1: override edges, base -> impl --------------------------------

    @Test
    fun anOverrideOfAReachableBaseIsReachable() {
        // Conservative virtual dispatch: something calls the base, so any
        // implementation may be what actually runs.
        val r = CocoReachability(
            Edges(
                calls = mapOf("a.Base::run/0" to listOf("a.C::main/0")),
                bases = mapOf("a.Impl::run/0" to listOf("a.Base::run/0")),
                known = setOf("a.Impl::run/0", "a.Base::run/0", "a.C::main/0"),
            ),
            roots = setOf("a.C::main/0"),
        )
        assertEquals(Reachability.REACHABLE, r.of("a.Impl::run/0"))
    }

    // --- rule 2: coverage-seeded roots ---------------------------------------

    @Test
    fun anExecutedMethodIsItselfARootEvenWithNoStaticCaller() {
        // Reflection and DI are invisible to a static graph. A method the
        // profile shows ran is ground truth: calling it dead would be flatly
        // false, and the report would be proposing to delete running code.
        val r = CocoReachability(
            Edges(known = setOf("a.C::viaReflection/0")),
            roots = setOf("a.C::viaReflection/0"),
        )
        assertEquals(Reachability.REACHABLE, r.of("a.C::viaReflection/0"))
    }

    @Test
    fun aMethodCalledOnlyFromAnExecutedMethodIsReachable() {
        val r = CocoReachability(
            Edges(calls = mapOf("a.C::helper/0" to listOf("a.C::ranViaDi/0")),
                  known = setOf("a.C::helper/0", "a.C::ranViaDi/0")),
            roots = setOf("a.C::ranViaDi/0"),
        )
        assertEquals(Reachability.REACHABLE, r.of("a.C::helper/0"))
    }

    // --- rule 3: the error direction -----------------------------------------

    @Test
    fun aMethodTheIndexHasNeverHeardOfIsUndeterminedNotDead() {
        // Absence from the index is absence of evidence. Reading it as "no
        // callers" would turn every indexing gap into a deletion proposal.
        val r = CocoReachability(Edges(known = setOf("a.C::main/0")), roots = setOf("a.C::main/0"))
        assertEquals(Reachability.UNKNOWN, r.of("a.C::ghost/0"))
    }

    @Test
    fun aNullKeyIsUndetermined() {
        val r = CocoReachability(Edges(), roots = emptySet())
        assertEquals(Reachability.UNKNOWN, r.of(null))
    }

    @Test
    fun withNoRootsAtAllNothingIsCalledDead() {
        // No entry point and no executed methods means the analysis has no
        // ground to stand on. Reporting everything unreachable would be a
        // catastrophic false positive.
        val r = CocoReachability(
            Edges(known = setOf("a.C::x/0", "a.C::y/0")),
            roots = emptySet(),
        )
        assertEquals(Reachability.UNKNOWN, r.of("a.C::x/0"))
    }

    // --- cycles and shape ----------------------------------------------------

    @Test
    fun aCallCycleDoesNotHangAndStaysUnreachableWhenDetached() {
        val r = CocoReachability(
            Edges(
                calls = mapOf("a.C::p/0" to listOf("a.C::q/0"), "a.C::q/0" to listOf("a.C::p/0")),
                known = setOf("a.C::p/0", "a.C::q/0", "a.C::main/0"),
            ),
            roots = setOf("a.C::main/0"),
        )
        assertEquals(Reachability.UNREACHABLE, r.of("a.C::p/0"))
    }

    @Test
    fun aCycleAttachedToARootIsReachable() {
        val r = CocoReachability(
            Edges(
                calls = mapOf(
                    "a.C::p/0" to listOf("a.C::q/0"),
                    "a.C::q/0" to listOf("a.C::p/0", "a.C::main/0"),
                ),
                known = setOf("a.C::p/0", "a.C::q/0", "a.C::main/0"),
            ),
            roots = setOf("a.C::main/0"),
        )
        assertEquals(Reachability.REACHABLE, r.of("a.C::p/0"))
    }
}
