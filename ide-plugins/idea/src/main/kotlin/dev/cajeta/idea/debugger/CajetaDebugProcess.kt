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
 * The XDebugProcess: launches `cajeta dap`, syncs IDE line breakpoints into the
 * session, drives the launch handshake, and on a `stopped` event pulls the
 * stack and parks the editor via positionReached. Each frame is wired with the
 * session so the Variables view can fetch its locals (CP6d, via
 * CajetaStackFrame.computeChildren). Value edit lands in CP6e.
 */
class CajetaDebugProcess(
    xSession: XDebugSession,
    private val configuration: CajetaRunConfiguration,
) : XDebugProcess(xSession) {

    private val log = Logger.getInstance(CajetaDebugProcess::class.java)
    private val processHandler = CajetaDebugProcessHandler()
    private val editorsProvider = CajetaDebuggerEditorsProvider()

    private val breakpointRegistry = BreakpointRegistry()
    private val breakpointHandler = CajetaBreakpointHandler(breakpointRegistry) { file ->
        // Live updates only after the handshake; the initial set seeds launch.
        // Push the file's breakpoints with their conditions (CP6f).
        if (launched) dapSession?.setBreakpoints(file, breakpointRegistry.breakpointsFor(file))
    }
    // CP6f-3b: break-on-throw toggle. Live updates after launch; the handler's
    // `armed` flag seeds the handshake.
    private val exceptionBreakpointHandler = CajetaExceptionBreakpointHandler { armed ->
        if (launched) dapSession?.setExceptionBreakpoints(armed)
    }

    // CP7-3: gutter glyphs summarizing the active bindings' memory facets.
    private val gutter = FacetGutterManager(xSession.project)

    private var process: Process? = null
    private var dapSession: CajetaDebugSession? = null

    @Volatile
    private var launched = false

    override fun getEditorsProvider(): XDebuggerEditorsProvider = editorsProvider

    override fun doGetProcessHandler(): ProcessHandler = processHandler

    override fun getBreakpointHandlers(): Array<XBreakpointHandler<*>> =
        arrayOf(breakpointHandler, exceptionBreakpointHandler)

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
            ds.onExited = { code -> gutter.clear(); processHandler.reportTerminated(code) }
            ds.onTerminated = { gutter.clear(); processHandler.reportTerminated(0) }
            ds.onOutput = { text -> processHandler.emitOutput(text) }
            ds.onClosed = { gutter.clear(); processHandler.reportTerminated(0) }
            ds.onStopped = { body -> onStopped(ds, body.opt("threadId")?.asInt() ?: 0) }

            ds.start()

            // Seed the launch handshake from whatever breakpoints the platform
            // registered before/at session start (with conditions, CP6f).
            val initialBreakpoints = breakpointRegistry.snapshotBreakpoints()
                .flatMap { (_, bps) -> bps }

            ds.launch(
                CajetaDebugSession.LaunchParams(
                    entryMethod = configuration.entryMethod,
                    sourceRoot = configuration.sourceRoot,
                    stopOnEntry = configuration.stopOnEntry,
                ),
                initialBreakpoints,
                // CP6f-3b: arm break-on-throw inside the handshake (before
                // configurationDone) if the user enabled it.
                exceptionBreakpoints = exceptionBreakpointHandler.armed,
            ).thenRun {
                launched = true
                // Reconcile any breakpoints registered during the handshake.
                breakpointRegistry.snapshotBreakpoints().forEach { (file, bps) ->
                    ds.setBreakpoints(file, bps)
                }
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

    /**
     * On a stop, build one execution stack per live thread/fiber for the
     * IntelliJ thread dropdown (CP6f-2c). The stopped thread's frames are
     * fetched up front so its stack is the active one and renders immediately;
     * every other thread becomes a lazy stack that fetches its own frames when
     * the user selects it. Falls back to a single stopped-thread stack if the
     * `threads` request fails.
     */
    private fun onStopped(ds: CajetaDebugSession, stoppedThreadId: Int) {
        ds.threads().thenCompose { threadsResponse ->
            val threads = CajetaDebugSession.parseThreads(threadsResponse)
            // Preload the stopped thread's frames (active stack must answer
            // getTopFrame synchronously); other stacks stay lazy.
            ds.stackTrace(stoppedThreadId).thenApply { stResponse ->
                val stoppedFrames = CajetaDebugSession.parseStackFrames(stResponse)
                    .map { CajetaStackFrame(it, resolvePosition(it), ds) }
                Pair(buildContext(ds, threads, stoppedThreadId, stoppedFrames), stoppedFrames)
            }
        }.thenAccept { (context, stoppedFrames) ->
            session.positionReached(context)
            // CP7-3: decorate the gutter at the stopped top frame's line using
            // the same locals the Variables view loads (FR-6.5).
            updateGutter(ds, stoppedFrames)
        }.exceptionally { e ->
            log.warn("building suspend context after stop failed", e)
            null
        }
    }

    /**
     * Refresh the gutter glyph for the stopped top frame. Fetches that frame's
     * locals via the same loadVariables path the Variables view uses; clears
     * the gutter if there's no source position to anchor to.
     */
    private fun updateGutter(ds: CajetaDebugSession, stoppedFrames: List<CajetaStackFrame>) {
        val top = stoppedFrames.firstOrNull()
        val pos = top?.sourcePosition
        if (top == null || pos == null) {
            gutter.clear()
            return
        }
        ds.loadVariables(top.frame.id).thenAccept { vars ->
            gutter.showAt(pos, vars)
        }.exceptionally {
            gutter.clear()
            null
        }
    }

    private fun buildContext(
        ds: CajetaDebugSession,
        threads: List<DapThread>,
        stoppedThreadId: Int,
        stoppedFrames: List<CajetaStackFrame>,
    ): CajetaSuspendContext {
        // Ensure the stopped thread is present (older adapter may report no
        // threads, or omit it); synthesize it if missing so there's always an
        // active stack to preload.
        val effective = threads.toMutableList()
        if (effective.none { it.id == stoppedThreadId }) {
            effective.add(0, DapThread(stoppedThreadId, "main"))
        }
        val stacks = effective.map { t ->
            if (t.id == stoppedThreadId) {
                CajetaExecutionStack(t.id, t.name, ds, ::resolvePosition, preloaded = stoppedFrames)
            } else {
                CajetaExecutionStack(t.id, t.name, ds, ::resolvePosition)
            }
        }
        val active = stacks.first { it.threadId == stoppedThreadId }
        return CajetaSuspendContext(active, stacks)
    }

    private fun resolvePosition(frame: DapStackFrame): XSourcePosition? {
        if (frame.path.isBlank() || frame.line <= 0) return null
        val file = LocalFileSystem.getInstance().findFileByPath(frame.path.replace('\\', '/'))
            ?: return null
        return XDebuggerUtil.getInstance().createPosition(file, frame.line - 1) // DAP line is 1-based
    }

    override fun resume(context: XSuspendContext?) {
        gutter.clear()   // CP7-3: stale facets vanish the moment we leave the stop.
        dapSession?.resume()
    }

    override fun startStepOver(context: XSuspendContext?) {
        // Real stepping is CP6e; for now resume to the next breakpoint.
        gutter.clear()
        dapSession?.resume()
    }

    override fun stop() {
        gutter.clear()
        dapSession?.disconnect()
        process?.destroyForcibly()
    }
}
