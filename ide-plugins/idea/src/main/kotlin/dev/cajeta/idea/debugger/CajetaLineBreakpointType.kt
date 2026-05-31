package dev.cajeta.idea.debugger

import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.xdebugger.breakpoints.XBreakpointProperties
import com.intellij.xdebugger.breakpoints.XLineBreakpointType
import dev.cajeta.idea.CajetaFileType

/**
 * Allows line breakpoints in `.cajeta` files. No per-breakpoint properties for
 * now; conditions and hit-counts arrive in CP6e.
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
