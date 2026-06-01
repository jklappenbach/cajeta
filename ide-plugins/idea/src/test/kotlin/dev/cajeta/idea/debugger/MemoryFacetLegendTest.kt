package dev.cajeta.idea.debugger

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Pure tests for the CP7-5 legend (FR-7.2/7.4). Checks the legend covers every
 * facet, states the color-independence guarantee, and — the invariant that
 * matters — uses exactly the same tag vocabulary [present]/[summarizeGutter]
 * emit, so the legend can't drift from the live decorations.
 */
class MemoryFacetLegendTest {

    @Test
    fun legendTextCoversAllSectionsAndAccessibilityNote() {
        val t = MemoryFacetLegend.text()
        assertTrue(t.contains("Ownership:"))
        assertTrue(t.contains("Allocation:"))
        assertTrue(t.contains("Lifetime:"))
        // FR-7.4: the legend states meaning never depends on color alone.
        assertTrue(t.contains("never depends on color alone"))
    }

    @Test
    fun ownershipLegendTermsMatchEmittedTags() {
        // The term column must equal the tags present() produces (no drift).
        assertEquals(
            listOf("owner", "borrow", "moved"),
            MemoryFacetLegend.ownership.map { it.term },
        )
        assertEquals(
            "owner",
            MemoryFacets(ownership = OwnershipRole.OWNER).present().tag,
        )
    }

    @Test
    fun allocationLegendTermsMatchEmittedTags() {
        assertEquals(
            listOf("stack", "heap", "shared"),
            MemoryFacetLegend.allocation.map { it.term },
        )
        assertEquals("heap", MemoryFacets(alloc = AllocClass.HEAP).present().tag)
        assertEquals("shared", MemoryFacets(alloc = AllocClass.SHARED).present().tag)
    }

    @Test
    fun lifetimeLegendCoversTheThreeStates() {
        assertEquals(
            listOf("live", "about-to-drop", "moved-out"),
            MemoryFacetLegend.lifetime.map { it.term },
        )
        // The two notable states appear in tags (live is intentionally omitted).
        assertTrue(
            MemoryFacets(ownership = OwnershipRole.OWNER, lifetime = LifetimeState.ABOUT_TO_DROP)
                .present().tag.contains("about-to-drop"),
        )
        assertTrue(
            MemoryFacets(ownership = OwnershipRole.OWNER, lifetime = LifetimeState.MOVED_OUT)
                .present().tag.contains("moved-out"),
        )
    }
}
