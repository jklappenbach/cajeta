package dev.cajeta.idea.debugger

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.PipedInputStream
import java.io.PipedOutputStream

/**
 * variable-inspection 4.2.6 — double-click on a LEAF value row starts Set Value
 * in place. Julian, live 2026-07-28: the platform binds Set Value to
 * right-click/F2 only, and its own double-click handler does nothing but
 * `expandIfEllipsis`, so a leaf double-click is a dead gesture.
 *
 * The listener itself needs a live XDebuggerTree, but the DECISION does not,
 * and the decision is the whole of the behaviour: intercept a leaf we can edit,
 * and NEVER an aggregate — expand/collapse on those must survive untouched.
 * Testing the decision rather than the mouse plumbing is deliberate: a
 * hand-driven click test would exercise Swing and leave the rule unstated.
 */
class VariablesInplaceEditTest {

    /**
     * A session is needed only so a value can carry a modifier at all — it is
     * never started, so no reader thread exists and there is nothing to tear
     * down (`DapClient.start()` is what spawns one).
     */
    private fun aSession(): CajetaDebugSession =
        CajetaDebugSession(DapClient(DapTransport(PipedInputStream(), PipedOutputStream())))

    private fun leaf(
        containerReference: Int = 3,
        facets: MemoryFacets = MemoryFacets.UNKNOWN,
    ) = CajetaValue(
        DapVariable("x", "3", "int32", variablesReference = 0, containerReference = containerReference, facets = facets),
        aSession(),
    )

    @Test
    fun anEditableLeafIsIntercepted() {
        assertTrue(VariablesInplaceEdit.shouldStartEdit(leaf()))
    }

    /**
     * The load-bearing negative: an aggregate double-click must fall through to
     * the tree so expand/collapse still works. Interception here would make the
     * Variables view feel broken in exchange for a feature nobody asked for on
     * that row.
     */
    @Test
    fun anAggregateIsNeverIntercepted() {
        val aggregate = CajetaValue(
            DapVariable("args", "{3 elements}", "cajeta.lang.String[]", variablesReference = 9, containerReference = 3),
            aSession(),
        )
        assertFalse(VariablesInplaceEdit.shouldStartEdit(aggregate))
    }

    /**
     * A leaf the server marked read-only has no modifier, so there is nothing
     * to start — a moved-out binding must not become editable by gesture when
     * it is not editable by menu (§4.1.6).
     */
    @Test
    fun aMovedOutLeafIsNotIntercepted() {
        val movedOut = leaf(facets = MemoryFacets(lifetime = LifetimeState.MOVED_OUT))
        assertFalse(VariablesInplaceEdit.shouldStartEdit(movedOut))
    }

    /** No container to target means setVariable has no address; not editable. */
    @Test
    fun aLeafWithNoContainerIsNotIntercepted() {
        assertFalse(VariablesInplaceEdit.shouldStartEdit(leaf(containerReference = 0)))
    }

    /** A detached value (no session) cannot round-trip an edit. */
    @Test
    fun aDetachedLeafIsNotIntercepted() {
        val detached = CajetaValue(DapVariable("x", "3", "int32", 0, containerReference = 3))
        assertFalse(VariablesInplaceEdit.shouldStartEdit(detached))
    }

    /** Another debugger's value in the same tree is none of our business. */
    @Test
    fun aForeignValueIsNotIntercepted() {
        assertFalse(VariablesInplaceEdit.shouldStartEdit(null))
    }

    /**
     * Two Cajeta sessions can run at once, so the global listener is
     * reference-counted: the first session attaches it and only the LAST one
     * detaches. An unbalanced release would kill the gesture in a session that
     * is still running — invisible until someone double-clicks and nothing
     * happens.
     */
    @Test
    fun theListenerSurvivesUntilTheLastSessionEnds() {
        assertFalse(VariablesInplaceEdit.watching)
        VariablesInplaceEdit.install()
        VariablesInplaceEdit.install()
        assertTrue(VariablesInplaceEdit.watching)

        VariablesInplaceEdit.uninstall()
        assertTrue("one session still open", VariablesInplaceEdit.watching)

        VariablesInplaceEdit.uninstall()
        assertFalse(VariablesInplaceEdit.watching)

        // Over-releasing must not go negative and strand the next install.
        VariablesInplaceEdit.uninstall()
        VariablesInplaceEdit.install()
        assertTrue(VariablesInplaceEdit.watching)
        VariablesInplaceEdit.uninstall()
        assertFalse(VariablesInplaceEdit.watching)
    }
}
