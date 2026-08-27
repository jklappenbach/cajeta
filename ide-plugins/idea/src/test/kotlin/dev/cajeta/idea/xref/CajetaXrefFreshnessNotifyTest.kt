package dev.cajeta.idea.xref

import com.intellij.testFramework.fixtures.BasePlatformTestCase
import dev.cajeta.idea.lint.XrefStream

/**
 * ide-symbol-index 9.2.3 — visible degradation means the indicator tracks the
 * state, and a StatusBarWidget only renders when something repaints it.
 *
 * Reported live 2026-08-24: the bar still read "xref stale" after a successful
 * index rebuild. The state machine was correct the whole time; nothing told the
 * status bar to redraw. So the contract under test is the NOTIFICATION, not the
 * text — the widget's own getText() already read live state and was never wrong
 * about what it was asked for.
 */
class CajetaXrefFreshnessNotifyTest : BasePlatformTestCase() {

    private fun freshness() = CajetaXrefFreshness.getInstance(project)

    fun testARealTransitionNotifies() {
        val f = freshness()
        var fired = 0
        val l: () -> Unit = { fired++ }
        f.addChangeListener(l)
        try {
            f.refreshStarted()                       // STALE -> REFRESHING
            assertEquals(1, fired)
            f.refreshSucceeded()                     // REFRESHING -> FRESH
            assertEquals(2, fired)
            f.refreshFailed("export refused")        // FRESH -> STALE
            assertEquals(3, fired)
        } finally {
            f.removeChangeListener(l)
        }
    }

    /**
     * The negative arm, and the reason `publish` compares before firing:
     * `updateFromLint` runs on EVERY edit's lint result. Repainting the status
     * bar per keystroke to redraw the same four characters is waste, so an
     * unchanged snapshot must stay silent.
     */
    fun testAnUnchangedSnapshotIsSilent() {
        val f = freshness()
        f.refreshSucceeded()
        var fired = 0
        val l: () -> Unit = { fired++ }
        f.addChangeListener(l)
        try {
            f.refreshSucceeded()
            f.refreshSucceeded()
            f.updateFromLint(compilerConfigured = true, stream = XrefStream.EMPTY)
            assertEquals("already FRESH — nothing to redraw", 0, fired)
        } finally {
            f.removeChangeListener(l)
        }
    }

    /** A differing REASON is a change: the tooltip carries it (9.2.3). */
    fun testTheReasonAloneIsAChange() {
        val f = freshness()
        f.refreshFailed("timed out")
        var fired = 0
        val l: () -> Unit = { fired++ }
        f.addChangeListener(l)
        try {
            f.refreshFailed("export refused")
            assertEquals(1, fired)
        } finally {
            f.removeChangeListener(l)
        }
    }

    fun testARemovedListenerStopsHearing() {
        val f = freshness()
        var fired = 0
        val l: () -> Unit = { fired++ }
        f.addChangeListener(l)
        f.refreshStarted()
        val seen = fired
        f.removeChangeListener(l)
        f.refreshSucceeded()
        assertEquals(seen, fired)
    }
}
