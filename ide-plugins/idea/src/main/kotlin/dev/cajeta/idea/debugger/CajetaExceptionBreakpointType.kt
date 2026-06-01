package dev.cajeta.idea.debugger

import com.intellij.xdebugger.breakpoints.XBreakpoint
import com.intellij.xdebugger.breakpoints.XBreakpointProperties
import com.intellij.xdebugger.breakpoints.XBreakpointType

/**
 * The "break on thrown exceptions" breakpoint (CP6f-3b). A single, non-line
 * breakpoint type — the user toggles it in the Breakpoints dialog (no source
 * position). When enabled, [CajetaExceptionBreakpointHandler] arms break-on-
 * throw over DAP (`setExceptionBreakpoints`), and the server parks at the
 * `__cajeta_throw` chokepoint with `stopped{reason:"exception"}`.
 *
 * Type filtering (break only on a chosen exception class) is a later cut; this
 * is the all-throws toggle the server advertises as the "all" filter.
 */
class CajetaExceptionBreakpointType :
    XBreakpointType<XBreakpoint<XBreakpointProperties<*>>, XBreakpointProperties<*>>(
        ID, "Cajeta Exception Breakpoints",
    ) {

    override fun getDisplayText(breakpoint: XBreakpoint<XBreakpointProperties<*>>): String =
        "Any thrown exception"

    override fun createProperties(): XBreakpointProperties<*>? = null

    /** Offer a default (disabled) entry so the toggle appears in the dialog. */
    override fun createDefaultBreakpoint(
        creator: XBreakpointCreator<XBreakpointProperties<*>>,
    ): XBreakpoint<XBreakpointProperties<*>> = creator.createBreakpoint(null)

    override fun isAddBreakpointButtonVisible(): Boolean = false

    companion object {
        const val ID = "cajeta-exception"
    }
}
