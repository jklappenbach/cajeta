package dev.cajeta.idea.usages

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ide-features 2.1.7 — Find Usages on an OVERRIDE must find the virtual call
 * sites that reach it.
 *
 * Found live on the tour 2026-08-30: Find Usages on any demo's `execute()`
 * reported nothing, while `Tour.cajeta:137` calls it through
 * `demos.stream().forEach((d) -> d.execute())`. A virtual call names the
 * STATIC receiver's method — `DemoClass::execute` — so an override has no
 * callers of its own.
 *
 * Neither query was broken: `overriddenBy` and `callersOf` both worked and
 * were both indexed. Nothing joined them. That is what this walk is, and it is
 * tested as a pure function so the ALGORITHM is checked rather than a
 * fabricated index — a fixture asserting what the code already believes is
 * exactly how the column-mismatch defect survived on both sides of this same
 * boundary a day earlier.
 */
class CajetaOverrideChainTest {

    private fun chainOf(
        start: String,
        edges: Map<String, List<String>>,
        maxDepth: Int = 32,
    ): List<String> =
        CajetaUsagesSearch.overriddenChain(start, maxDepth) { edges[it].orEmpty() }

    @Test
    fun theTourShape_anOverrideReachesItsBase() {
        val edges = mapOf(
            "tour.lang.ClassesDemo::execute(pointer)" to
                listOf("tour.DemoClass::execute(pointer)"))
        assertEquals(
            listOf("tour.DemoClass::execute(pointer)"),
            chainOf("tour.lang.ClassesDemo::execute(pointer)", edges))
    }

    @Test
    fun aMethodOverridingNothingHasAnEmptyChain() {
        assertEquals(emptyList<String>(), chainOf("demo.A::solo(pointer)", emptyMap()))
    }

    @Test
    fun theWalkIsTransitive() {
        // C overrides B overrides A. A call site on A reaches C, so Find
        // Usages on C must reach A — one level would miss it.
        val edges = mapOf(
            "demo.C::run(pointer)" to listOf("demo.B::run(pointer)"),
            "demo.B::run(pointer)" to listOf("demo.A::run(pointer)"))
        assertEquals(
            listOf("demo.B::run(pointer)", "demo.A::run(pointer)"),
            chainOf("demo.C::run(pointer)", edges))
    }

    @Test
    fun aDiamondReportsEachAncestorOnce() {
        // A class implementing two interfaces that share a root must not list
        // the root twice — the usage view would show duplicate call sites.
        val edges = mapOf(
            "demo.D::go(pointer)" to listOf("demo.L::go(pointer)", "demo.R::go(pointer)"),
            "demo.L::go(pointer)" to listOf("demo.Root::go(pointer)"),
            "demo.R::go(pointer)" to listOf("demo.Root::go(pointer)"))
        val chain = chainOf("demo.D::go(pointer)", edges)
        assertEquals(3, chain.size)
        assertEquals(1, chain.count { it == "demo.Root::go(pointer)" })
        assertTrue(chain.containsAll(
            listOf("demo.L::go(pointer)", "demo.R::go(pointer)", "demo.Root::go(pointer)")))
    }

    @Test
    fun aCycleTerminatesInsteadOfHangingTheIde() {
        // A well-formed export cannot describe this. It walks machine-generated
        // data on the search thread, so a corrupt index must not freeze the
        // IDE — the guard is what makes that true, not the data being good.
        val edges = mapOf(
            "demo.X::m(pointer)" to listOf("demo.Y::m(pointer)"),
            "demo.Y::m(pointer)" to listOf("demo.X::m(pointer)"))
        val chain = chainOf("demo.X::m(pointer)", edges)
        assertEquals(listOf("demo.Y::m(pointer)"), chain)
    }

    @Test
    fun selfReferenceIsNotReportedAsItsOwnAncestor() {
        val edges = mapOf("demo.S::m(pointer)" to listOf("demo.S::m(pointer)"))
        assertEquals(emptyList<String>(), chainOf("demo.S::m(pointer)", edges))
    }

    @Test
    fun depthIsCappedSoACorruptIndexCannotRunAway() {
        // A chain longer than the cap truncates rather than walking forever.
        val edges = (0 until 100).associate {
            "demo.C$it::m(pointer)" to listOf("demo.C${it + 1}::m(pointer)")
        }
        val chain = chainOf("demo.C0::m(pointer)", edges, maxDepth = 4)
        assertEquals(4, chain.size)
        assertEquals("demo.C1::m(pointer)", chain.first())
    }
}
