package dev.cajeta.idea.debugger

import com.intellij.openapi.diagnostic.Logger
import com.intellij.xdebugger.frame.XValue
import com.intellij.xdebugger.impl.ui.tree.SetValueInplaceEditor
import com.intellij.xdebugger.impl.ui.tree.XDebuggerTree
import com.intellij.xdebugger.impl.ui.tree.nodes.XValueNodeImpl
import java.awt.AWTEvent
import java.awt.Toolkit
import java.awt.event.AWTEventListener
import java.awt.event.MouseEvent
import javax.swing.SwingUtilities

/**
 * variable-inspection 4.2.6 — double-click a LEAF value row to edit it in place.
 *
 * The platform binds Set Value to the context menu and F2 only; its own
 * double-click handler on the Variables tree does nothing but expand an
 * ellipsis node, so double-clicking a scalar is a dead gesture (Julian, live
 * 2026-07-28). This adds the gesture WITHOUT taking anything away: the rule
 * fires only on a leaf we could already edit from the menu, so aggregates keep
 * their expand/collapse and every other debugger's rows are left alone.
 *
 * Scoping is by VALUE, not by tree: [shouldStartEdit] answers false for
 * anything that is not one of our own [CajetaValue]s. That is what makes it
 * safe to watch a tree the platform owns and shares with other providers.
 */
object VariablesInplaceEdit {

    private val log = Logger.getInstance(VariablesInplaceEdit::class.java)

    /**
     * The whole rule. A double-click starts an edit when the row is one of our
     * values, is a LEAF (the server minted no expansion handle for it), and is
     * editable — which already encodes read-only for a moved-out binding and
     * for a value with no container to write through.
     */
    fun shouldStartEdit(value: XValue?): Boolean {
        val v = value as? CajetaValue ?: return false
        return v.isLeaf && v.modifier != null
    }

    private var listener: AWTEventListener? = null
    private var sessions = 0

    /** Test seam: is the global listener currently attached? */
    @get:Synchronized
    val watching: Boolean get() = listener != null

    /**
     * Start watching for the gesture, and stop when the last Cajeta session
     * ends. This watches MOUSE_CLICKED globally rather than attaching to one
     * tree, because there is no hook for tree creation: the Variables tree is
     * built lazily, the watches tree is a second one, an Inspect popup is a
     * third, and all of them are recreated across the session's life. Finding
     * "the" tree at any single moment gets the gesture on some rows and not
     * others.
     *
     * The breadth costs nothing and risks nothing: a non-tree source is three
     * cheap checks away from being dropped, and [shouldStartEdit] refuses every
     * value that is not ours, so no other debugger's rows can be affected.
     * Reference-counted, since two Cajeta sessions can run at once.
     */
    @Synchronized
    fun install() {
        sessions++
        if (listener != null) return
        val l = AWTEventListener { e ->
            if (e is MouseEvent &&
                e.id == MouseEvent.MOUSE_CLICKED &&
                e.clickCount == 2 &&
                SwingUtilities.isLeftMouseButton(e)
            ) {
                (e.source as? XDebuggerTree)?.let { handle(it, e) }
            }
        }
        Toolkit.getDefaultToolkit().addAWTEventListener(l, AWTEvent.MOUSE_EVENT_MASK)
        listener = l
    }

    /** Balances one [install]; the listener goes away with the last session. */
    @Synchronized
    fun uninstall() {
        if (sessions > 0) sessions--
        if (sessions > 0) return
        listener?.let { Toolkit.getDefaultToolkit().removeAWTEventListener(it) }
        listener = null
    }

    /**
     * Returns true only when an edit was started. The platform's own
     * double-click handler has already run by the time an AWT listener sees the
     * event and does nothing on a leaf (it only expands an ellipsis node), so
     * claiming a leaf here takes nothing away from the tree.
     */
    private fun handle(tree: XDebuggerTree, event: MouseEvent): Boolean {
        val path = tree.getPathForLocation(event.x, event.y) ?: return false
        val node = path.lastPathComponent as? XValueNodeImpl ?: return false
        if (!shouldStartEdit(node.xValue)) return false
        val name = node.name ?: return false
        return try {
            SetValueInplaceEditor.show(node, name)
            true
        } catch (t: Throwable) {
            // An impl-side signature change must degrade to the platform's
            // behaviour, never take down the Variables view on a click.
            log.warn("cajeta: in-place edit on double-click failed", t)
            false
        }
    }
}
