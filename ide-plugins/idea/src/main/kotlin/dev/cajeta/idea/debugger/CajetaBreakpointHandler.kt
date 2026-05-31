package dev.cajeta.idea.debugger

import com.intellij.xdebugger.breakpoints.XBreakpointHandler
import com.intellij.xdebugger.breakpoints.XBreakpointProperties
import com.intellij.xdebugger.breakpoints.XLineBreakpoint

/**
 * Syncs IDE line breakpoints into the DAP session. The platform calls
 * register/unregister as breakpoints are added/removed (including at session
 * start for pre-existing ones); each call updates the shared
 * [BreakpointRegistry] and notifies [onFileChanged] with the file's new line
 * set so the process can push a whole-file setBreakpoints when the session is
 * live (before launch, the registry alone seeds the handshake).
 */
class CajetaBreakpointHandler(
    private val registry: BreakpointRegistry,
    private val onFileChanged: (file: String, lines: List<Int>) -> Unit,
) : XBreakpointHandler<XLineBreakpoint<XBreakpointProperties<*>>>(CajetaLineBreakpointType::class.java) {

    override fun registerBreakpoint(breakpoint: XLineBreakpoint<XBreakpointProperties<*>>) {
        val position = breakpoint.sourcePosition ?: return
        val file = position.file.path
        val lines = registry.add(file, position.line + 1) // DAP lines are 1-based
        onFileChanged(file, lines)
    }

    override fun unregisterBreakpoint(
        breakpoint: XLineBreakpoint<XBreakpointProperties<*>>,
        temporary: Boolean,
    ) {
        val position = breakpoint.sourcePosition ?: return
        val file = position.file.path
        val lines = registry.remove(file, position.line + 1)
        onFileChanged(file, lines)
    }
}
