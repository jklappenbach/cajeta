package dev.cajeta.idea.settings

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
}
