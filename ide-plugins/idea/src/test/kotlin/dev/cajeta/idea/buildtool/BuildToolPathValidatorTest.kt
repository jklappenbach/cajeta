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

    @Test
    fun aVersionBelowTheFloorWarnsAndNamesBothVersions() {
        val w = BuildToolPathValidator.coverageFloorWarning("cajeta 0.21.0 (7c0f40f3)")
        assertNotNull(w)
        assertTrue("names what was found: $w", w!!.contains("0.21.0"))
        assertTrue("names the floor: $w", w.contains(BuildToolPathValidator.COCO_MIN_TOOLCHAIN))
        assertTrue("names the symptom so the stack is attributable: $w", w.contains("value=0x3"))
    }

    @Test
    fun theFloorItselfAndNewerDoNotWarn() {
        assertNull(BuildToolPathValidator.coverageFloorWarning("cajeta 0.21.1 (d43749d9)"))
        assertNull(BuildToolPathValidator.coverageFloorWarning("cajeta 0.22.0 (a61fa0ca)"))
        assertNull(BuildToolPathValidator.coverageFloorWarning("cajeta 1.0.0 (deadbeef)"))
    }

    @Test
    fun comparisonIsNumericNotLexicographic() {
        // "0.9.4" > "0.21.1" as strings, and that is the whole point.
        assertTrue(BuildToolPathValidator.isBelow("0.9.4", "0.21.1"))
        assertTrue(BuildToolPathValidator.isBelow("0.21.0", "0.21.1"))
        assertFalse(BuildToolPathValidator.isBelow("0.21.1", "0.21.1"))
        assertFalse(BuildToolPathValidator.isBelow("0.100.0", "0.21.1"))
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
