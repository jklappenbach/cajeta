package dev.cajeta.idea.buildtool

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

// --- coco's toolchain floor ------------------------------------------------
//
// The plugin is AOT-compiled by the toolchain that runs it, so an old build
// tool makes every coco verb throw `value=0x3` from whichever method reads a
// file first — a stack that names coco and no version at all. These pin the
// comparison, because the interesting case is 0.9.x vs 0.21.1, where a string
// compare gets the answer backwards.

class BuildToolCoverageFloorTest {

    // Versions are derived from COCO_MIN_TOOLCHAIN rather than written out.
    // The floor moves — 0.21.1 when coco's plugin was miscompiled by an older
    // toolchain, 0.22.2 when coco stopped shelling out to llc — and a test
    // that hardcodes it fails on the bump for no reason anyone learns from.
    private val floor = BuildToolPathValidator.COCO_MIN_TOOLCHAIN
    private fun bump(v: String, delta: Int): String {
        val p = v.split('.').map { it.toInt() }.toMutableList()
        p[p.size - 1] = p.last() + delta
        return p.joinToString(".")
    }

    @Test
    fun aVersionBelowTheFloorWarnsAndNamesBothVersions() {
        val below = bump(floor, -1)
        val w = BuildToolPathValidator.coverageFloorWarning("cajeta $below (7c0f40f3)")
        assertNotNull(w)
        assertTrue("names what was found: $w", w!!.contains(below))
        assertTrue("names the floor: $w", w.contains(floor))
        assertTrue("names the symptom so the stack is attributable: $w", w.contains("value=0x3"))
    }

    @Test
    fun theFloorItselfAndNewerDoNotWarn() {
        assertNull(BuildToolPathValidator.coverageFloorWarning("cajeta $floor (d43749d9)"))
        assertNull(BuildToolPathValidator.coverageFloorWarning("cajeta ${bump(floor, 1)} (a61fa0ca)"))
        assertNull(BuildToolPathValidator.coverageFloorWarning("cajeta 1.0.0 (deadbeef)"))
    }

    @Test
    fun comparisonIsNumericNotLexicographic() {
        // "0.9.4" > "0.21.1" as strings, and that is the whole point.
        assertTrue(BuildToolPathValidator.isBelow("0.9.4", "0.21.1"))
        assertTrue(BuildToolPathValidator.isBelow("0.21.0", "0.21.1"))
        assertFalse(BuildToolPathValidator.isBelow("0.21.1", "0.21.1"))
        assertFalse(BuildToolPathValidator.isBelow("0.100.0", "0.21.1"))
        // These are fixed on purpose: they pin the COMPARISON, not the floor.
    }

    @Test
    fun unrecognisedOutputIsSilentRatherThanCryingWolf() {
        // A tool that does not answer --version in the expected shape is a
        // different problem, reported by validate(). Warning here as well
        // would train the reader to ignore the one line that matters.
        assertNull(BuildToolPathValidator.coverageFloorWarning(""))
        assertNull(BuildToolPathValidator.coverageFloorWarning("command not found"))
        assertNull(BuildToolPathValidator.coverageFloorWarning("rustc 1.79.0"))
    }

    @Test
    fun theVersionIsParsedOutOfARealVersionLine() {
        assertEquals("0.21.0", BuildToolPathValidator.parseVersion("cajeta 0.21.0 (7c0f40f3)"))
        assertEquals("0.22.1", BuildToolPathValidator.parseVersion("cajeta 0.22.1 (ebe72518)"))
        assertNull(BuildToolPathValidator.parseVersion("not a version line"))
    }
}
