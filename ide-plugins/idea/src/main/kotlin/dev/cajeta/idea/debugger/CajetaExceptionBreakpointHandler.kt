package dev.cajeta.idea.debugger

import com.intellij.xdebugger.breakpoints.XBreakpoint
import com.intellij.xdebugger.breakpoints.XBreakpointHandler
import com.intellij.xdebugger.breakpoints.XBreakpointProperties

/**
 * Syncs the Cajeta exception breakpoint into the DAP session (CP6f-3b). The
 * platform calls register/unregister as the user enables/disables break-on-
 * throw (including at session start for a pre-existing one); each call updates
 * the shared armed flag and notifies [onArmedChanged] so the process can push
 * `setExceptionBreakpoints` when the session is live (before launch, the flag
 * alone seeds the handshake).
 *
 * The all-throws toggle is a single boolean today; multiple distinct exception
 * breakpoints all collapse to "armed", so a count tracks how many are active.
 */
class CajetaExceptionBreakpointHandler(
    private val onArmedChanged: (armed: Boolean) -> Unit,
) : XBreakpointHandler<XBreakpoint<XBreakpointProperties<*>>>(
    CajetaExceptionBreakpointType::class.java,
) {
    private var activeCount = 0

    override fun registerBreakpoint(breakpoint: XBreakpoint<XBreakpointProperties<*>>) {
        activeCount++
        if (activeCount == 1) onArmedChanged(true)
    }

    override fun unregisterBreakpoint(
        breakpoint: XBreakpoint<XBreakpointProperties<*>>,
        temporary: Boolean,
    ) {
        if (activeCount > 0) activeCount--
        if (activeCount == 0) onArmedChanged(false)
    }

    /** Current armed state — used to seed the launch handshake. */
    val armed: Boolean get() = activeCount > 0
}
