package dev.cajeta.idea.settings

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Pure test of the persisted settings defaults (CP7-5, FR-7.3): the three
 * memory-facet visualization surfaces are independent boolean toggles, each on
 * out of the box. State is a plain data class, so this needs no platform.
 */
class CajetaSettingsStateTest {

    @Test
    fun facetTogglesDefaultOnAndIndependent() {
        val s = CajetaSettings.State()
        assertTrue(s.showFacetsInVariables)
        assertTrue(s.showFacetsInGutter)
        assertTrue(s.showFacetsInline)

        // Independent fields: flipping one leaves the others untouched.
        val onlyGutterOff = s.copy(showFacetsInGutter = false)
        assertTrue(onlyGutterOff.showFacetsInVariables)
        assertTrue(!onlyGutterOff.showFacetsInGutter)
        assertTrue(onlyGutterOff.showFacetsInline)
    }

    /**
     * variable-inspection §3.1.4: the expansion page size is a persisted
     * setting defaulting to 50 rows. 50 is the spec's number and is NOT the
     * server's hard fallback (100) — the two are deliberately independent, so
     * an unset plugin still gets a working page and a configured plugin always
     * wins. A test that read the default off the server would pass while the
     * setting did nothing.
     */
    @Test
    fun debugPageSizeDefaultsToFifty() {
        assertEquals(50, CajetaSettings.State().debugPageSize)
        assertEquals(50, CajetaSettings.DEFAULT_DEBUG_PAGE_SIZE)
    }

    @Test
    fun debugPageSizeIsIndependentOfTheOtherFields() {
        val s = CajetaSettings.State()
        val paged = s.copy(debugPageSize = 200)
        assertEquals(200, paged.debugPageSize)
        assertEquals(s.compilerPath, paged.compilerPath)
        assertTrue(paged.useLintServer)
        assertTrue(paged.showFacetsInVariables)
    }
}
