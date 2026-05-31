package dev.cajeta.idea.debugger

import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.xdebugger.breakpoints.XBreakpointProperties
import com.intellij.xdebugger.breakpoints.XLineBreakpointType
import dev.cajeta.idea.CajetaFileType

/**
 * Allows line breakpoints in `.cajeta` files. Conditions are supported (CP6f):
 * the platform's default conditional-breakpoint UI applies because this type
 * has no custom properties, and the condition expression rides into the DAP
 * setBreakpoints via [CajetaBreakpointHandler]. Hit-counts are not yet wired.
 */
class CajetaLineBreakpointType :
    XLineBreakpointType<XBreakpointProperties<*>>(ID, "Cajeta Line Breakpoints") {

    override fun createBreakpointProperties(file: VirtualFile, line: Int): XBreakpointProperties<*>? = null

    override fun canPutAt(file: VirtualFile, line: Int, project: Project): Boolean =
        file.fileType == CajetaFileType

    companion object {
        const val ID = "cajeta-line"
    }
}
