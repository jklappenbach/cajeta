package dev.cajeta.idea.debugger

import com.intellij.xdebugger.breakpoints.XBreakpointHandler
import com.intellij.xdebugger.breakpoints.XBreakpointProperties
import com.intellij.xdebugger.breakpoints.XLineBreakpoint

/**
 * Syncs IDE line breakpoints into the DAP session. The platform calls
 * register/unregister as breakpoints are added/removed (including at session
 * start for pre-existing ones); each call updates the shared
 * [BreakpointRegistry] and notifies [onFileChanged] with the file so the process
 * can push a whole-file setBreakpoints when the session is live (before launch,
 * the registry alone seeds the handshake).
 *
 * CP6f: a breakpoint's condition (the expression typed in the IDE's breakpoint
 * dialog) is captured into the registry so it rides along into setBreakpoints.
 */
class CajetaBreakpointHandler(
    private val registry: BreakpointRegistry,
    private val onFileChanged: (file: String) -> Unit,
) : XBreakpointHandler<XLineBreakpoint<XBreakpointProperties<*>>>(CajetaLineBreakpointType::class.java) {

    override fun registerBreakpoint(breakpoint: XLineBreakpoint<XBreakpointProperties<*>>) {
        val position = breakpoint.sourcePosition ?: return
        val file = position.file.path
        val condition = breakpoint.conditionExpression?.expression ?: ""
        registry.add(file, position.line + 1, condition) // DAP lines are 1-based
        onFileChanged(file)
    }

    override fun unregisterBreakpoint(
        breakpoint: XLineBreakpoint<XBreakpointProperties<*>>,
        temporary: Boolean,
    ) {
        val position = breakpoint.sourcePosition ?: return
        val file = position.file.path
        registry.remove(file, position.line + 1)
        onFileChanged(file)
    }
}
