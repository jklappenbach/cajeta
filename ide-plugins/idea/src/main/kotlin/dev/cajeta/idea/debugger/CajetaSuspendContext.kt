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

class CajetaExecutionStack(private val frames: List<CajetaStackFrame>) :
    XExecutionStack("Main") {

    override fun getTopFrame(): XStackFrame? = frames.firstOrNull()

    override fun computeStackFrames(firstFrameIndex: Int, container: XStackFrameContainer) {
        if (firstFrameIndex < frames.size) {
            container.addStackFrames(frames.subList(firstFrameIndex, frames.size), true)
        } else {
            container.addStackFrames(emptyList(), true)
        }
    }
}

class CajetaSuspendContext(private val stack: CajetaExecutionStack) : XSuspendContext() {

    override fun getActiveExecutionStack(): XExecutionStack = stack

    override fun getExecutionStacks(): Array<XExecutionStack> = arrayOf(stack)
}
