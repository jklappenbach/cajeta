package dev.cajeta.idea.debugger

import com.intellij.execution.process.ProcessHandler
import com.intellij.openapi.diagnostic.Logger
import com.intellij.xdebugger.XDebugProcess
import com.intellij.xdebugger.XDebugSession
import com.intellij.xdebugger.breakpoints.XBreakpointHandler
import com.intellij.xdebugger.evaluation.XDebuggerEditorsProvider
import com.intellij.xdebugger.frame.XSuspendContext
import dev.cajeta.idea.settings.CajetaSettings
import java.io.File

/**
 * The XDebugProcess skeleton (CP6b): on session start it launches `cajeta dap`
 * and drives the initialize -> launch -> configurationDone handshake through
 * [CajetaDebugSession], then maps DAP exit/termination back to the debugger
 * UI's process lifecycle.
 *
 * Without breakpoints (CP6c) the program runs straight to completion under the
 * debugger. Line breakpoints, suspend contexts, and the variables view layer
 * on top in CP6c-e; [getBreakpointHandlers] is empty for now.
 */
class CajetaDebugProcess(
    session: XDebugSession,
    private val configuration: CajetaRunConfiguration,
) : XDebugProcess(session) {

    private val log = Logger.getInstance(CajetaDebugProcess::class.java)
    private val processHandler = CajetaDebugProcessHandler()
    private val editorsProvider = CajetaDebuggerEditorsProvider()

    private var process: Process? = null
    private var dapSession: CajetaDebugSession? = null

    override fun getEditorsProvider(): XDebuggerEditorsProvider = editorsProvider

    override fun doGetProcessHandler(): ProcessHandler = processHandler

    override fun sessionInitialized() {
        val binary = CajetaSettings.instance.compilerPath
        if (binary.isBlank() || !File(binary).canExecute()) {
            processHandler.emitOutput(
                "Cajeta compiler not found at '$binary'. " +
                    "Set it in Settings | Languages & Frameworks | Cajeta.\n",
            )
            processHandler.startNotify()
            processHandler.reportTerminated(-1)
            return
        }

        try {
            val proc = CajetaDapLauncher(binary, CajetaDapLauncher.defaultDllDir()).start()
            process = proc
            val ds = CajetaDebugSession(DapClient(DapTransport(proc.inputStream, proc.outputStream)))
            dapSession = ds

            processHandler.onDestroy = { ds.disconnect() }
            ds.onExited = { code -> processHandler.reportTerminated(code) }
            ds.onTerminated = { processHandler.reportTerminated(0) }
            ds.onOutput = { text -> processHandler.emitOutput(text) }
            ds.onClosed = { processHandler.reportTerminated(0) }

            ds.start()
            ds.launch(
                CajetaDebugSession.LaunchParams(
                    entryMethod = configuration.entryMethod,
                    sourceRoot = configuration.sourceRoot,
                    stopOnEntry = configuration.stopOnEntry,
                ),
            ).exceptionally { e ->
                log.warn("cajeta dap launch failed", e)
                processHandler.emitOutput("launch failed: ${e.message}\n")
                processHandler.reportTerminated(-1)
                null
            }
        } catch (e: Exception) {
            log.warn("failed to start cajeta dap", e)
            processHandler.emitOutput("failed to start cajeta dap: ${e.message}\n")
            processHandler.reportTerminated(-1)
        }
        processHandler.startNotify()
    }

    override fun resume(context: XSuspendContext?) {
        dapSession?.resume()
    }

    override fun stop() {
        dapSession?.disconnect()
        process?.destroyForcibly()
    }

    override fun getBreakpointHandlers(): Array<XBreakpointHandler<*>> = emptyArray()
}
