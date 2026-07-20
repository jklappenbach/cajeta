package dev.cajeta.idea.debugger

import com.intellij.execution.process.ProcessHandler
import com.intellij.execution.process.ProcessOutputTypes
import java.io.OutputStream

/**
 * ProcessHandler representing the debuggee carried by `cajeta dap`. The DAP
 * server runs the program in-process, so there's no console pipe to attach;
 * this handler exists to drive the debugger UI's lifecycle (running ->
 * terminated) and to surface any program output forwarded over DAP `output`
 * events.
 */
class CajetaDebugProcessHandler : ProcessHandler() {

    /** Invoked when the user stops/detaches; used to disconnect the DAP session. */
    @Volatile
    var onDestroy: (() -> Unit)? = null

    override fun destroyProcessImpl() {
        onDestroy?.invoke()
        notifyProcessTerminated(0)
    }

    override fun detachProcessImpl() {
        onDestroy?.invoke()
        notifyProcessDetached()
    }

    override fun detachIsDefault(): Boolean = false

    override fun getProcessInput(): OutputStream? = null

    fun reportTerminated(exitCode: Int) {
        if (!isProcessTerminated) notifyProcessTerminated(exitCode)
    }

    fun emitOutput(text: String) {
        notifyTextAvailable(text, ProcessOutputTypes.STDOUT)
    }

    fun emitError(text: String) {
        notifyTextAvailable(text, ProcessOutputTypes.STDERR)
    }
}
