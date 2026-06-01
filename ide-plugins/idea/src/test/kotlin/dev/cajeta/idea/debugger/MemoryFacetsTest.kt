package dev.cajeta.idea.debugger

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Pure-core tests for the CP7-2 facet decoding + presentation mapping. No
 * IntelliJ platform fixture — this is the plain mapping the FR requires
 * unit-tested directly (FR-8.4); the icon/attribute translation in CajetaValue
 * is the only platform-touching part and is intentionally trivial.
 */
class MemoryFacetsTest {

    // ---- tag parsing ----

    @Test
    fun enumsParseFromWireTags() {
        assertEquals(AllocClass.STACK, AllocClass.fromTag("stack"))
        assertEquals(AllocClass.HEAP, AllocClass.fromTag("heap"))
        assertEquals(AllocClass.SHARED, AllocClass.fromTag("shared"))
        assertEquals(AllocClass.UNKNOWN, AllocClass.fromTag("unknown"))
        assertEquals(AllocClass.UNKNOWN, AllocClass.fromTag(null))

        assertEquals(OwnershipRole.OWNER, OwnershipRole.fromTag("owner"))
        assertEquals(OwnershipRole.BORROW, OwnershipRole.fromTag("borrow"))
        assertEquals(OwnershipRole.MOVED, OwnershipRole.fromTag("moved"))
        assertEquals(OwnershipRole.UNKNOWN, OwnershipRole.fromTag("nonsense"))

        assertEquals(LifetimeState.LIVE, LifetimeState.fromTag("live"))
        assertEquals(LifetimeState.MOVED_OUT, LifetimeState.fromTag("moved-out"))
        assertEquals(LifetimeState.ABOUT_TO_DROP, LifetimeState.fromTag("about-to-drop"))
        assertEquals(LifetimeState.UNKNOWN, LifetimeState.fromTag(null))
    }

    @Test
    fun parseNullObjectIsUnknown() {
        val f = MemoryFacets.parse(null)
        assertEquals(MemoryFacets.UNKNOWN, f)
        assertFalse(f.isKnown)
    }

    @Test
    fun parseDecodesAllThreeAxes() {
        val cajeta = Json.obj(
            "alloc" to Json.of("heap"),
            "ownership" to Json.of("owner"),
            "lifetime" to Json.of("about-to-drop"),
        )
        val f = MemoryFacets.parse(cajeta)
        assertEquals(AllocClass.HEAP, f.alloc)
        assertEquals(OwnershipRole.OWNER, f.ownership)
        assertEquals(LifetimeState.ABOUT_TO_DROP, f.lifetime)
        assertTrue(f.isKnown)
    }

    @Test
    fun parseToleratesMissingKeys() {
        val f = MemoryFacets.parse(Json.obj("alloc" to Json.of("stack")))
        assertEquals(AllocClass.STACK, f.alloc)
        assertEquals(OwnershipRole.UNKNOWN, f.ownership)
        assertEquals(LifetimeState.UNKNOWN, f.lifetime)
        assertTrue(f.isKnown)   // one known facet is enough
    }

    // ---- presentation mapping ----

    @Test
    fun ownedHeapAboutToDropIsBoldWithFullTag() {
        val p = MemoryFacets(AllocClass.HEAP, OwnershipRole.OWNER, LifetimeState.ABOUT_TO_DROP).present()
        assertEquals("owner · heap · about-to-drop", p.tag)
        assertTrue(p.bold)
        assertFalse(p.strikeout)
        assertFalse(p.error)
        assertTrue(p.tooltip.contains("owned"))
        assertTrue(p.tooltip.contains("heap-allocated"))
        assertTrue(p.tooltip.contains("about to drop"))
    }

    @Test
    fun liveBorrowOmitsLifetimeFromTag() {
        val p = MemoryFacets(AllocClass.HEAP, OwnershipRole.BORROW, LifetimeState.LIVE).present()
        assertEquals("borrow · heap", p.tag)   // `live` is the unremarkable case
        assertFalse(p.bold)
        assertFalse(p.strikeout)
        assertTrue(p.tooltip.contains("borrowed"))
        assertTrue(p.tooltip.contains("live"))
    }

    @Test
    fun movedOutIsStruckGrayedErrorAndNotBold() {
        val p = MemoryFacets(AllocClass.HEAP, OwnershipRole.OWNER, LifetimeState.MOVED_OUT).present()
        assertEquals("owner · heap · moved-out", p.tag)
        assertFalse(p.bold)        // moved-out overrides the owner emphasis
        assertTrue(p.strikeout)
        assertTrue(p.grayed)
        assertTrue(p.error)
        assertTrue(p.tooltip.contains("reading is an error"))
    }

    @Test
    fun stackPrimitiveTagsAllocOnly() {
        val p = MemoryFacets(AllocClass.STACK, OwnershipRole.UNKNOWN, LifetimeState.LIVE).present()
        assertEquals("stack", p.tag)
        assertFalse(p.bold)
        assertFalse(p.error)
    }

    @Test
    fun fullyUnknownHasEmptyTagAndNeutralTooltip() {
        val p = MemoryFacets.UNKNOWN.present()
        assertEquals("", p.tag)
        assertEquals("no ownership metadata", p.tooltip)
        assertFalse(p.bold)
        assertFalse(p.strikeout)
        assertFalse(p.error)
    }

    // ---- decode flows through parseVariables ----

    @Test
    fun parseVariablesCarriesFacets() {
        val response = Json.obj(
            "body" to Json.obj(
                "variables" to Json.arr(
                    Json.obj(
                        "name" to Json.of("o"),
                        "value" to Json.of("<demo.Foo@0x1>"),
                        "type" to Json.of("demo.Foo"),
                        "variablesReference" to Json.of(0),
                        "cajeta" to Json.obj(
                            "alloc" to Json.of("heap"),
                            "ownership" to Json.of("owner"),
                            "lifetime" to Json.of("about-to-drop"),
                        ),
                    ),
                ),
            ),
        )
        val vars = CajetaDebugSession.parseVariables(response)
        assertEquals(1, vars.size)
        assertEquals(AllocClass.HEAP, vars[0].facets.alloc)
        assertEquals(OwnershipRole.OWNER, vars[0].facets.ownership)
        assertEquals(LifetimeState.ABOUT_TO_DROP, vars[0].facets.lifetime)
    }

    // ---- gutter summary (CP7-3) ----

    private fun varWith(name: String, facets: MemoryFacets) =
        DapVariable(name = name, value = "", type = "", variablesReference = 0, facets = facets)

    @Test
    fun gutterSummaryNullWhenNothingKnown() {
        assertEquals(null, summarizeGutter(emptyList()))
        assertEquals(
            null,
            summarizeGutter(listOf(varWith("x", MemoryFacets.UNKNOWN))),
        )
    }

    @Test
    fun gutterSummaryPicksMostSignificantOwnershipAndAlloc() {
        val vars = listOf(
            varWith("b", MemoryFacets(AllocClass.STACK, OwnershipRole.BORROW, LifetimeState.LIVE)),
            varWith("o", MemoryFacets(AllocClass.HEAP, OwnershipRole.OWNER, LifetimeState.ABOUT_TO_DROP)),
        )
        val s = summarizeGutter(vars)!!
        assertEquals(OwnershipRole.OWNER, s.ownership)   // owner outranks borrow
        assertEquals(AllocClass.HEAP, s.alloc)            // heap outranks stack
        assertFalse(s.anyMovedOut)
        assertEquals(2, s.count)
        assertTrue(s.tooltip.contains("o — owner · heap · about-to-drop"))
        assertTrue(s.tooltip.contains("b — borrow · stack"))
    }

    @Test
    fun gutterSummaryFlagsMovedOut() {
        val vars = listOf(
            varWith("m", MemoryFacets(AllocClass.HEAP, OwnershipRole.OWNER, LifetimeState.MOVED_OUT)),
        )
        val s = summarizeGutter(vars)!!
        assertTrue(s.anyMovedOut)
    }

    @Test
    fun gutterSummarySharedOutranksHeap() {
        val vars = listOf(
            varWith("h", MemoryFacets(AllocClass.HEAP, OwnershipRole.UNKNOWN, LifetimeState.LIVE)),
            varWith("s", MemoryFacets(AllocClass.SHARED, OwnershipRole.UNKNOWN, LifetimeState.LIVE)),
        )
        assertEquals(AllocClass.SHARED, summarizeGutter(vars)!!.alloc)
    }

    @Test
    fun gutterSummaryIgnoresUnknownBindings() {
        val vars = listOf(
            varWith("u", MemoryFacets.UNKNOWN),
            varWith("o", MemoryFacets(AllocClass.HEAP, OwnershipRole.OWNER, LifetimeState.LIVE)),
        )
        val s = summarizeGutter(vars)!!
        assertEquals(1, s.count)   // only the known binding is summarized
        assertFalse(s.tooltip.contains("\nu"))
    }

    @Test
    fun parseVariablesWithoutCajetaIsUnknown() {
        val response = Json.obj(
            "body" to Json.obj(
                "variables" to Json.arr(
                    Json.obj(
                        "name" to Json.of("x"),
                        "value" to Json.of("5"),
                        "type" to Json.of("int32"),
                        "variablesReference" to Json.of(0),
                    ),
                ),
            ),
        )
        val vars = CajetaDebugSession.parseVariables(response)
        assertFalse(vars[0].facets.isKnown)
    }
}
