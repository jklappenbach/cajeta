package dev.cajeta.idea.markdown

import dev.cajeta.idea.markdown.MarkdownRenderingToggle.EditorOp
import dev.cajeta.idea.settings.CajetaSettings
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Pure test of the "Toggle Markdown Rendering in Comments" action's decision
 * core (the W3b deferred menu action). The action flips
 * CajetaSettings.renderMarkdownInComments and then applies the resulting state
 * to every open editor; opFor() is the per-editor branch it drives — render
 * ON installs the fold regions, OFF tears them down so comments revert to raw
 * source. Plain data/logic, so no platform fixture.
 */
class MarkdownRenderingToggleTest {

    @Test
    fun renderingDefaultsOn() {
        // The toggle's "selected" state mirrors this default out of the box.
        assertTrue(CajetaSettings.State().renderMarkdownInComments)
    }

    @Test
    fun opForMapsEnabledToInstallDisabledToUninstall() {
        assertEquals(EditorOp.INSTALL, MarkdownRenderingToggle.opFor(enabled = true))
        assertEquals(EditorOp.UNINSTALL, MarkdownRenderingToggle.opFor(enabled = false))
    }

    @Test
    fun flippingTwiceReturnsToStart() {
        var enabled = true
        enabled = MarkdownRenderingToggle.flip(enabled)
        assertEquals(false, enabled)
        enabled = MarkdownRenderingToggle.flip(enabled)
        assertEquals(true, enabled)
    }
}
