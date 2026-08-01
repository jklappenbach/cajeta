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

    // Live breakpoints by (file BASENAME, 1-based line) — the key the server
    // matches on, so a `verified: false` report can be mapped back to the
    // gutter marker that has to change. Basename, not full path: the server
    // arms by basename and echoes that back.
    private val tracked =
        java.util.concurrent.ConcurrentHashMap<
            Pair<String, Int>, XLineBreakpoint<XBreakpointProperties<*>>>()

    private fun key(file: String, line: Int) =
        java.io.File(file).name to line

    override fun registerBreakpoint(breakpoint: XLineBreakpoint<XBreakpointProperties<*>>) {
        val position = breakpoint.sourcePosition ?: return
        val file = position.file.path
        val condition = breakpoint.conditionExpression?.expression ?: ""
        registry.add(file, position.line + 1, condition) // DAP lines are 1-based
        tracked[key(file, position.line + 1)] = breakpoint
        onFileChanged(file)
    }

    override fun unregisterBreakpoint(
        breakpoint: XLineBreakpoint<XBreakpointProperties<*>>,
        temporary: Boolean,
    ) {
        val position = breakpoint.sourcePosition ?: return
        val file = position.file.path
        registry.remove(file, position.line + 1)
        tracked.remove(key(file, position.line + 1))
        onFileChanged(file)
    }

    /** The live breakpoint at [file] (path or basename) and 1-based [line]. */
    fun find(file: String, line: Int): XLineBreakpoint<XBreakpointProperties<*>>? =
        tracked[key(file, line)]
}
