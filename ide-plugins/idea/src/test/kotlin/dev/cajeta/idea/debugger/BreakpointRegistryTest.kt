package dev.cajeta.idea.debugger

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** Unit tests for the per-file breakpoint line tracking. */
class BreakpointRegistryTest {

    @Test
    fun addKeepsLinesSortedAndDeduped() {
        val r = BreakpointRegistry()
        assertEquals(listOf(6), r.add("Calc.cajeta", 6))
        assertEquals(listOf(4, 6), r.add("Calc.cajeta", 4))
        assertEquals(listOf(4, 6), r.add("Calc.cajeta", 6)) // duplicate is a no-op
    }

    @Test
    fun removeReturnsRemainingAndDropsEmptyFiles() {
        val r = BreakpointRegistry()
        r.add("Calc.cajeta", 4)
        r.add("Calc.cajeta", 6)
        assertEquals(listOf(6), r.remove("Calc.cajeta", 4))
        assertTrue(r.remove("Calc.cajeta", 6).isEmpty())
        assertTrue("emptied file should leave the snapshot", r.snapshot().isEmpty())
    }

    @Test
    fun snapshotIsPerFile() {
        val r = BreakpointRegistry()
        r.add("A.cajeta", 1)
        r.add("B.cajeta", 2)
        r.add("B.cajeta", 5)
        val snap = r.snapshot()
        assertEquals(listOf(1), snap["A.cajeta"])
        assertEquals(listOf(2, 5), snap["B.cajeta"])
    }

    @Test
    fun removingUnknownFileIsHarmless() {
        val r = BreakpointRegistry()
        assertTrue(r.remove("Nope.cajeta", 3).isEmpty())
        assertTrue(r.linesFor("Nope.cajeta").isEmpty())
    }
}
