package dev.cajeta.idea.buildtool

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * W-buildtool unit 11.1.1: the pure action-state core for the tool-window
 * toolbar (spec §9). Enable/disable logic and the grouping toggle live here so
 * the toolbar (a thin `ActionToolbar`) has no decision logic of its own.
 */
class BuildToolbarStateTest {

    @Test
    fun stopEnabledOnlyWhenRunsActive() {
        assertFalse(BuildToolbarState(activeRuns = 0).stopEnabled)
        assertTrue(BuildToolbarState(activeRuns = 1).stopEnabled)
        assertTrue(BuildToolbarState(activeRuns = 5).stopEnabled)
    }

    @Test
    fun refreshAlwaysEnabledRunAndContextNeedRunnableSelection() {
        val none = BuildToolbarState(hasTasks = true, selectionRunnable = false)
        assertTrue("refresh always available", none.refreshEnabled)
        assertFalse("no runnable selection -> Run disabled", none.runSelectedEnabled)

        val sel = BuildToolbarState(hasTasks = true, selectionRunnable = true)
        assertTrue(sel.runSelectedEnabled)
    }

    @Test
    fun expandCollapseAndRunPickerNeedTasks() {
        val empty = BuildToolbarState(hasTasks = false)
        assertFalse(empty.expandCollapseEnabled)
        assertFalse(empty.runTaskPickerEnabled)

        val populated = BuildToolbarState(hasTasks = true)
        assertTrue(populated.expandCollapseEnabled)
        assertTrue(populated.runTaskPickerEnabled)
    }

    @Test
    fun groupingToggleFlipsAndIsReflected() {
        val grouped = BuildToolbarState(groupByProject = true)
        assertTrue(grouped.groupByProject)
        assertFalse(grouped.toggledGrouping().groupByProject)
        // round-trip back
        assertTrue(grouped.toggledGrouping().toggledGrouping().groupByProject)
    }
}
