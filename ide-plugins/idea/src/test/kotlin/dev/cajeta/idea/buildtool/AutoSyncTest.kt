package dev.cajeta.idea.buildtool

import dev.cajeta.idea.settings.CajetaSettings
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * W-buildtool unit 15.1.1: the pure auto-sync decision + debounce core (spec
 * §13). A watched-manifest change maps (per the auto-reload setting) to ignore /
 * reload / prompt, rapid changes coalesce within a debounce window, and a failed
 * re-discovery keeps the prior tree (§13.2.3). No `com.intellij.*`.
 */
class AutoSyncTest {

    @Test
    fun decideMapsAutoReloadSetting() {
        assertEquals(AutoSync.Action.RELOAD, AutoSync.decide(CajetaSettings.AUTO_RELOAD_ALWAYS))
        assertEquals(AutoSync.Action.IGNORE, AutoSync.decide(CajetaSettings.AUTO_RELOAD_NEVER))
        assertEquals(AutoSync.Action.PROMPT, AutoSync.decide(CajetaSettings.AUTO_RELOAD_PROMPT))
        assertEquals(AutoSync.Action.PROMPT, AutoSync.decide("anything-else"))   // default: prompt, like Gradle
    }

    @Test
    fun reconcileKeepsPriorTreeOnTotalFailure() {
        val prior = mapOf("/a/cajeta.json" to "modelA")
        // a successful re-discovery replaces
        assertEquals(mapOf("/a/cajeta.json" to "modelA2"), AutoSync.reconcile(prior, mapOf("/a/cajeta.json" to "modelA2")))
        // a total failure (nothing discovered) keeps the prior tree usable (§13.2.3)
        assertEquals(prior, AutoSync.reconcile(prior, emptyMap()))
    }

    @Test
    fun debounceCoalescesChangesWithinWindow() {
        val d = Debounce(windowMs = 300)
        d.onChange(nowMs = 1000)
        assertFalse("not due before the window elapses", d.isDue(1200))
        d.onChange(nowMs = 1250)                    // a second change resets the window
        assertFalse("window reset by the later change", d.isDue(1450))
        assertTrue("due once the window elapses from the last change", d.isDue(1600))
        d.consume()
        assertFalse("not due again until the next change", d.isDue(2000))
    }
}
