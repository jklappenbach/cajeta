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
    private val configuration: CajetaDebugLaunchSpec,
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

    // CP7-3/4: gutter glyphs + inline hints summarizing the active bindings'
    // memory facets at a stop. Both sourced from the same loadVariables.
    private val gutter = FacetGutterManager(xSession.project)
    private val inlay = FacetInlayManager(xSession.project)

    private var process: Process? = null
    private var dapSession: CajetaDebugSession? = null

    @Volatile
    private var launched = false

    // The fiber the last `stopped` event parked (dap-stepping): step requests
    // must name the stopped thread — the server rejects any other.
    @Volatile
    private var stoppedThreadId = 0

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

        // Mount the compiler's stdlib source in the background (idempotent,
        // cheap on cache hit) so a stop in a stdlib frame opens a real file
        // (ide-symbol-index 8.2.4).
        com.intellij.openapi.application.ApplicationManager.getApplication()
            .executeOnPooledThread {
                dev.cajeta.idea.xref.CajetaSourceMountGlue.ensureStdlibMounted()
            }

        try {
            // Resident lifecycle (resident-debug-server §2): the project
            // service owns ONE `cajeta dap` per project — reused across
            // sessions, respawned when dead or when the binary setting
            // moved, killed with the project. The service's stderr pump
            // outlives sessions; rebind its sink to THIS session's console.
            val resident = session.project.getService(
                CajetaResidentDapService::class.java)
            resident.stderrSink = { processHandler.emitError(it) }
            val server = resident.acquire(binary)
            process = server.process
            val ds = CajetaDebugSession(server.client)
            ds.ownsClient = false   // the service owns the shared client
            dapSession = ds

            wireSessionCallbacks(ds)
            ds.start()

            // Seed the launch handshake from whatever breakpoints the platform
            // registered before/at session start (with conditions, CP6f).
            val initialBreakpoints = breakpointRegistry.snapshotBreakpoints()
                .flatMap { (_, bps) -> bps }

            val params = CajetaDebugSession.LaunchParams(
                entryMethod = configuration.entryMethod,
                sourceRoot = configuration.sourceRoot,
                stopOnEntry = configuration.stopOnEntry,
                envVars = configuration.envVars,
                inheritSystemEnv = configuration.inheritSystemEnv,
                // fast-debug-launch 5.2.2: the shared project cache tree;
                // the server serves a whole-program slot on a warm launch.
                cacheDir = session.project.basePath
                    ?.let { "$it/.cajeta/cache" } ?: "",
                // resident-debug-server 5.2.1/4.2.1: identity + residency.
                compilerPath = binary,
                resident = true,
            )
            ds.launch(
                params,
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
                // Identity refusal exits the server (spec 5.2.1): pay the
                // respawn HERE so the same Debug click cold-starts the new
                // compiler instead of failing once.
                log.warn("cajeta dap launch failed; respawning once", e)
                resident.releaseForRespawn()
                try {
                    resident.stderrSink = { processHandler.emitError(it) }
                    val fresh = resident.acquire(binary)
                    process = fresh.process
                    val retry = CajetaDebugSession(fresh.client)
                    retry.ownsClient = false
                    dapSession = retry
                    wireSessionCallbacks(retry)
                    retry.start()
                    retry.launch(
                        params, initialBreakpoints,
                        exceptionBreakpoints = exceptionBreakpointHandler.armed,
                    ).thenRun {
                        launched = true
                        breakpointRegistry.snapshotBreakpoints().forEach { (file, bps) ->
                            retry.setBreakpoints(file, bps)
                        }
                    }.exceptionally { e2 ->
                        log.warn("cajeta dap relaunch failed", e2)
                        processHandler.emitOutput("launch failed: ${e2.message}\n")
                        processHandler.reportTerminated(-1)
                        null
                    }
                } catch (e2: Exception) {
                    log.warn("cajeta dap respawn failed", e2)
                    processHandler.emitOutput("launch failed: ${e2.message}\n")
                    processHandler.reportTerminated(-1)
                }
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
            // CP7-3/4: decorate the gutter + inline at the stopped top frame's
            // line using the same locals the Variables view loads (FR-6.5).
            updateDecorations(ds, stoppedFrames)
        }.exceptionally { e ->
            log.warn("building suspend context after stop failed", e)
            null
        }
    }

    /**
     * Refresh the gutter glyph + inline hint for the stopped top frame. Fetches
     * that frame's locals via the same loadVariables path the Variables view
     * uses; clears the decorations if there's no source position to anchor to.
     */
    private fun updateDecorations(ds: CajetaDebugSession, stoppedFrames: List<CajetaStackFrame>) {
        val top = stoppedFrames.firstOrNull()
        val pos = top?.sourcePosition
        if (top == null || pos == null) {
            clearDecorations()
            return
        }
        ds.loadVariables(top.frame.id).thenAccept { vars ->
            gutter.showAt(pos, vars)
            inlay.showAt(pos, vars)
        }.exceptionally {
            clearDecorations()
            null
        }
    }

    private fun clearDecorations() {
        gutter.clear()
        inlay.clear()
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
        // A library/stdlib frame reports the path the COMPILER knew — root-
        // relative or absolute on the build machine. When it does not exist
        // locally, the mounted twin does (ide-symbol-index 8.2.4).
        val file = LocalFileSystem.getInstance().findFileByPath(frame.path.replace('\\', '/'))
            ?: dev.cajeta.idea.xref.CajetaMountedSources.findMountedBySuffix(frame.path)
            ?: return null
        return XDebuggerUtil.getInstance().createPosition(file, frame.line - 1) // DAP line is 1-based
    }

    override fun resume(context: XSuspendContext?) {
        clearDecorations()   // CP7-3/4: stale facets vanish the moment we leave the stop.
        dapSession?.resume()
    }

    override fun startStepOver(context: XSuspendContext?) {
        clearDecorations()
        dapSession?.stepOver(stoppedThreadId)
    }

    override fun startStepInto(context: XSuspendContext?) {
        clearDecorations()
        dapSession?.stepInto(stoppedThreadId)
    }

    override fun startStepOut(context: XSuspendContext?) {
        clearDecorations()
        dapSession?.stepOut(stoppedThreadId)
    }

    /** Per-session callback wiring, shared by the first launch and the
     *  identity-refusal respawn retry. `onDestroy` DISCONNECTS only — the
     *  resident server (project-owned) survives the session. */
    private fun wireSessionCallbacks(ds: CajetaDebugSession) {
        processHandler.onDestroy = { ds.disconnect() }
        ds.onExited = { code -> clearDecorations(); processHandler.reportTerminated(code) }
        ds.onTerminated = { clearDecorations(); processHandler.reportTerminated(0) }
        ds.onOutput = { text -> processHandler.emitOutput(text) }
        ds.onClosed = { clearDecorations(); processHandler.reportTerminated(0) }
        ds.onStopped = { body ->
            val tid = body.opt("threadId")?.asInt() ?: 0
            stoppedThreadId = tid
            onStopped(ds, tid)
        }
    }

    override fun stop() {
        clearDecorations()
        // End the SESSION; the resident server stays for the next one
        // (resident-debug-server §2). A hung debuggee is unstuck by
        // releaseForRespawn from the service, not by killing here.
        dapSession?.disconnect()
    }
}
