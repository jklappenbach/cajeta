package dev.cajeta.idea.debugger

import com.intellij.execution.process.ProcessHandler
import com.intellij.openapi.diagnostic.Logger
import com.intellij.openapi.vfs.LocalFileSystem
import com.intellij.xdebugger.XDebugProcess
import com.intellij.xdebugger.XDebugSession
import com.intellij.xdebugger.XDebuggerUtil
import com.intellij.xdebugger.XSourcePosition
import com.intellij.xdebugger.breakpoints.XBreakpointHandler
import com.intellij.xdebugger.evaluation.XDebuggerEditorsProvider
import com.intellij.xdebugger.frame.XSuspendContext
import dev.cajeta.idea.settings.CajetaSettings
import java.io.File

/**
 * The XDebugProcess (CP6c): launches `cajeta dap`, syncs IDE line breakpoints
 * into the session, drives the launch handshake, and on a `stopped` event
 * pulls the stack and parks the editor via positionReached. Variables view
 * (computeChildren) and value edit land in CP6d/e.
 */
class CajetaDebugProcess(
    xSession: XDebugSession,
    private val configuration: CajetaRunConfiguration,
) : XDebugProcess(xSession) {

    private val log = Logger.getInstance(CajetaDebugProcess::class.java)
    private val processHandler = CajetaDebugProcessHandler()
    private val editorsProvider = CajetaDebuggerEditorsProvider()

    private val breakpointRegistry = BreakpointRegistry()
    private val breakpointHandler = CajetaBreakpointHandler(breakpointRegistry) { file, lines ->
        // Live updates only after the handshake; the initial set seeds launch.
        if (launched) dapSession?.setBreakpoints(file, lines)
    }

    private var process: Process? = null
    private var dapSession: CajetaDebugSession? = null

    @Volatile
    private var launched = false

    override fun getEditorsProvider(): XDebuggerEditorsProvider = editorsProvider

    override fun doGetProcessHandler(): ProcessHandler = processHandler

    override fun getBreakpointHandlers(): Array<XBreakpointHandler<*>> = arrayOf(breakpointHandler)

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
            ds.onStopped = { _ -> onStopped(ds) }

            ds.start()

            // Seed the launch handshake from whatever breakpoints the platform
            // registered before/at session start.
            val initialBreakpoints = breakpointRegistry.snapshot()
                .flatMap { (file, lines) -> lines.map { CajetaDebugSession.LineBreakpoint(file, it) } }

            ds.launch(
                CajetaDebugSession.LaunchParams(
                    entryMethod = configuration.entryMethod,
                    sourceRoot = configuration.sourceRoot,
                    stopOnEntry = configuration.stopOnEntry,
                ),
                initialBreakpoints,
            ).thenRun {
                launched = true
                // Reconcile any breakpoints registered during the handshake.
                breakpointRegistry.snapshot().forEach { (file, lines) -> ds.setBreakpoints(file, lines) }
            }.exceptionally { e ->
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

    private fun onStopped(ds: CajetaDebugSession) {
        ds.stackTrace().thenAccept { response ->
            val frames = CajetaDebugSession.parseStackFrames(response)
                .map { CajetaStackFrame(it, resolvePosition(it)) }
            val context = CajetaSuspendContext(CajetaExecutionStack(frames))
            session.positionReached(context)
        }.exceptionally { e ->
            log.warn("stackTrace after stop failed", e)
            null
        }
    }

    private fun resolvePosition(frame: DapStackFrame): XSourcePosition? {
        if (frame.path.isBlank() || frame.line <= 0) return null
        val file = LocalFileSystem.getInstance().findFileByPath(frame.path.replace('\\', '/'))
            ?: return null
        return XDebuggerUtil.getInstance().createPosition(file, frame.line - 1) // DAP line is 1-based
    }

    override fun resume(context: XSuspendContext?) {
        dapSession?.resume()
    }

    override fun startStepOver(context: XSuspendContext?) {
        // Real stepping is CP6e; for now resume to the next breakpoint.
        dapSession?.resume()
    }

    override fun stop() {
        dapSession?.disconnect()
        process?.destroyForcibly()
    }
}
