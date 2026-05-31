package dev.cajeta.idea.debugger

import com.intellij.icons.AllIcons
import com.intellij.ui.ColoredTextContainer
import com.intellij.ui.SimpleTextAttributes
import com.intellij.xdebugger.XSourcePosition
import com.intellij.xdebugger.frame.XExecutionStack
import com.intellij.xdebugger.frame.XStackFrame
import com.intellij.xdebugger.frame.XSuspendContext

/**
 * Debugger-UI view of a parked program. CP6c shows the call stack with each
 * frame's source position so the editor parks on the line; per-frame variables
 * (computeChildren) arrive in CP6d.
 */
class CajetaStackFrame(
    val frame: DapStackFrame,
    private val position: XSourcePosition?,
) : XStackFrame() {

    override fun getSourcePosition(): XSourcePosition? = position

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
