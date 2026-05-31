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

    @Test
    fun carriesConditionPerLine() {
        val r = BreakpointRegistry()
        r.add("Calc.cajeta", 6, "a == 6")
        r.add("Calc.cajeta", 4) // unconditional
        val bps = r.breakpointsFor("Calc.cajeta")
        assertEquals(listOf(4, 6), bps.map { it.line })       // still sorted
        assertEquals("", bps.first { it.line == 4 }.condition)
        assertEquals("a == 6", bps.first { it.line == 6 }.condition)
    }

    @Test
    fun reAddingALineUpdatesItsCondition() {
        val r = BreakpointRegistry()
        r.add("Calc.cajeta", 6, "a == 1")
        r.add("Calc.cajeta", 6, "a == 2") // same line, new condition
        val bps = r.breakpointsFor("Calc.cajeta")
        assertEquals(1, bps.size)
        assertEquals("a == 2", bps[0].condition)
    }

    @Test
    fun snapshotBreakpointsIsPerFileWithConditions() {
        val r = BreakpointRegistry()
        r.add("A.cajeta", 1, "x > 0")
        r.add("B.cajeta", 2)
        val snap = r.snapshotBreakpoints()
        assertEquals("x > 0", snap.getValue("A.cajeta").single().condition)
        assertEquals("", snap.getValue("B.cajeta").single().condition)
    }
}
