package dev.cajeta.idea.debugger

import com.intellij.icons.AllIcons
import com.intellij.ui.ColoredTextContainer
import com.intellij.ui.SimpleTextAttributes
import com.intellij.xdebugger.XSourcePosition
import com.intellij.xdebugger.frame.XCompositeNode
import com.intellij.xdebugger.frame.XExecutionStack
import com.intellij.xdebugger.frame.XStackFrame
import com.intellij.xdebugger.frame.XSuspendContext
import com.intellij.xdebugger.frame.XValueChildrenList

/**
 * Debugger-UI view of a parked program. Shows the call stack with each frame's
 * source position (CP6c) and, when a [session] is wired, the frame's locals in
 * the Variables view via [computeChildren] (CP6d). The scopes/variables fetch
 * lives in [CajetaDebugSession.loadVariables]; this frame only renders.
 */
class CajetaStackFrame(
    val frame: DapStackFrame,
    private val position: XSourcePosition?,
    private val session: CajetaDebugSession? = null,
) : XStackFrame() {

    override fun getSourcePosition(): XSourcePosition? = position

    override fun computeChildren(node: XCompositeNode) {
        val ds = session
        if (ds == null) {
            node.addChildren(XValueChildrenList.EMPTY, true)
            return
        }
        ds.loadVariables(frame.id).thenAccept { vars ->
            val children = XValueChildrenList()
            for (v in vars) children.add(v.name, CajetaValue(v, ds))
            node.addChildren(children, true)
        }.exceptionally {
            node.setErrorMessage("Failed to load variables: ${it.message}")
            null
        }
    }

    /** Stable key so the UI keeps frame selection across steps. */
    override fun getEqualityObject(): Any = "${frame.path}:${frame.line}:${frame.name}"

    override fun customizePresentation(component: ColoredTextContainer) {
        component.append(frame.name, SimpleTextAttributes.REGULAR_ATTRIBUTES)
        if (frame.line > 0) {
            val base = frame.path.substringAfterLast('/').substringAfterLast('\\')
            val where = if (base.isNotEmpty()) "  ($base:${frame.line})" else "  (line ${frame.line})"
            component.append(where, SimpleTextAttributes.GRAYED_ATTRIBUTES)
        }
        component.setIcon(AllIcons.Debugger.Frame)
    }
}

/**
 * One thread/fiber's call stack in the IntelliJ thread dropdown (CP6f-2c).
 *
 * IntelliJ calls [getTopFrame] synchronously when it builds the dropdown, so a
 * stack's frames must be available without blocking. The **stopped** thread is
 * therefore constructed with its frames already in hand ([preloaded]); every
 * other thread is lazy — it fetches `stackTrace(threadId)` only when the user
 * selects it, in [computeStackFrames] (which IntelliJ already calls off the EDT
 * via a callback). A lazy stack reports no top frame until expanded, which is
 * the platform's expected "not yet computed" state.
 */
class CajetaExecutionStack(
    val threadId: Int,
    displayName: String,
    private val session: CajetaDebugSession?,
    private val resolvePosition: (DapStackFrame) -> XSourcePosition?,
    private val preloaded: List<CajetaStackFrame>? = null,
) : XExecutionStack(displayName) {

    override fun getTopFrame(): XStackFrame? = preloaded?.firstOrNull()

    override fun computeStackFrames(firstFrameIndex: Int, container: XStackFrameContainer) {
        // The stopped thread already has its frames; serve them synchronously.
        preloaded?.let {
            container.addStackFrames(it.drop(firstFrameIndex), true)
            return
        }
        val ds = session
        if (ds == null) {
            container.addStackFrames(emptyList(), true)
            return
        }
        // Lazy: fetch this thread's frames on demand. Frame ids are global
        // (server keys the per-stop frame table by threadId), so scopes/
        // variables on these frames round-trip exactly like the stopped thread.
        ds.stackTrace(threadId).thenAccept { response ->
            val frames = CajetaDebugSession.parseStackFrames(response)
                .map { CajetaStackFrame(it, resolvePosition(it), ds) }
            container.addStackFrames(frames.drop(firstFrameIndex), true)
        }.exceptionally { e ->
            container.errorOccurred("Failed to load frames for thread $threadId: ${e.message}")
            null
        }
    }
}

/**
 * The set of all thread/fiber stacks at a stop (CP6f-2c). [active] is the
 * stopped thread (shown first in the dropdown); [all] holds every thread,
 * active included, in the order the server reported them.
 */
class CajetaSuspendContext(
    private val active: CajetaExecutionStack,
    private val all: List<CajetaExecutionStack>,
) : XSuspendContext() {

    override fun getActiveExecutionStack(): XExecutionStack = active

    override fun getExecutionStacks(): Array<XExecutionStack> = all.toTypedArray()
}
