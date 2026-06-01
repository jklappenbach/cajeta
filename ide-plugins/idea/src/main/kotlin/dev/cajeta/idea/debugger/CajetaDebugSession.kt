package dev.cajeta.idea.debugger

import java.util.concurrent.CompletableFuture

/**
 * Orchestrates the DAP session lifecycle over a [DapClient], independent of
 * the IntelliJ platform so it can be driven end-to-end against the real
 * `cajeta dap` server in a plain JUnit test. The XDebugProcess (CP6b platform
 * glue) is a thin delegate over this.
 *
 * Lifecycle: [start] wires event handlers, then [launch] runs the
 * initialize -> launch -> setBreakpoints -> configurationDone handshake (the
 * order the cajeta server accepts). The server then JIT-runs the program;
 * `stopped`/`exited`/`terminated`/`output` events fire the callbacks.
 *
 * Stack frames ([stackTrace]/[parseStackFrames]) and per-frame locals
 * ([loadVariables], scopes -> variables) are decoded here so the platform layer
 * just renders them; value edit (setVariable) arrives in CP6e.
 */
class CajetaDebugSession(private val client: DapClient) {

    data class LaunchParams(
        val entryMethod: String,
        val sourceRoot: String,
        val stopOnEntry: Boolean = false,
    )

    /** A line breakpoint, optionally conditional (CP6f). A blank condition is
     *  unconditional. The condition grammar the server supports is constrained:
     *  `<localName> <op> <literal>`, op ∈ { ==, !=, <, <=, >, >= }. */
    data class LineBreakpoint(val file: String, val line: Int, val condition: String = "")

    /** Raw `stopped` event body (reason, threadId). Stack mapping is CP6d. */
    @Volatile var onStopped: ((Json) -> Unit)? = null
    @Volatile var onTerminated: (() -> Unit)? = null
    @Volatile var onExited: ((Int) -> Unit)? = null
    @Volatile var onOutput: ((String) -> Unit)? = null
    @Volatile var onClosed: (() -> Unit)? = null

    fun start() {
        client.onEvent("stopped") { ev -> onStopped?.invoke(ev.opt("body") ?: Json.obj()) }
        client.onEvent("terminated") { onTerminated?.invoke() }
        client.onEvent("exited") { ev ->
            val code = ev.opt("body")?.opt("exitCode")?.asInt() ?: 0
            onExited?.invoke(code)
        }
        client.onEvent("output") { ev ->
            ev.opt("body")?.opt("output")?.let { onOutput?.invoke(it.asString()) }
        }
        client.onClosed = { onClosed?.invoke() }
        client.start()
    }

    /**
     * initialize -> launch -> setBreakpoints (grouped by file) ->
     * configurationDone. The returned future completes when configurationDone
     * is acknowledged (the program then begins executing on the server).
     */
    fun launch(
        params: LaunchParams,
        breakpoints: List<LineBreakpoint> = emptyList(),
    ): CompletableFuture<Void> {
        var chain = client.sendRequest(
            "initialize",
            Json.obj("adapterID" to Json.of("cajeta")),
        ).thenCompose {
            client.sendRequest("launch", launchArgs(params))
        }
        for ((file, fileBreakpoints) in breakpoints.groupBy { it.file }) {
            chain = chain.thenCompose {
                client.sendRequest("setBreakpoints", breakpointArgs(file, fileBreakpoints))
            }
        }
        return chain
            .thenCompose { client.sendRequest("configurationDone") }
            .thenApply { null }
    }

    /**
     * Edit a variable in place. [variablesReference] is the containing scope's
     * ref (frameId+1); the server writes through to the live fiber's stack slot
     * and returns the re-rendered value, which is what this future resolves to.
     * Completes exceptionally on a DAP error (e.g. unknown name / bad value).
     */
    fun setVariable(variablesReference: Int, name: String, value: String): CompletableFuture<String> =
        client.sendRequest(
            "setVariable",
            Json.obj(
                "variablesReference" to Json.of(variablesReference),
                "name" to Json.of(name),
                "value" to Json.of(value),
            ),
        ).thenApply { it.at("body").at("value").asString() }

    /** DAP `continue` — resume the parked program. */
    fun resume(): CompletableFuture<Json> = client.sendRequest("continue")

    /**
     * Replace the breakpoints for one source file (DAP setBreakpoints is
     * whole-file-replace). Used for live add/remove once the session is running;
     * the initial set goes through [launch]. This unconditional overload takes
     * bare lines; the [LineBreakpoint] overload carries per-line conditions.
     */
    fun setBreakpoints(file: String, lines: List<Int>): CompletableFuture<Json> =
        setBreakpoints(file, lines.map { LineBreakpoint(file, it) })

    /** Conditional variant of [setBreakpoints]: replace [file]'s breakpoints,
     *  each optionally carrying a condition (CP6f). */
    @JvmName("setBreakpointsConditional")
    fun setBreakpoints(file: String, breakpoints: List<LineBreakpoint>): CompletableFuture<Json> =
        client.sendRequest("setBreakpoints", breakpointArgs(file, breakpoints))

    /** Raw stackTrace response (stopped thread); structured via [parseStackFrames]. */
    fun stackTrace(): CompletableFuture<Json> = client.sendRequest("stackTrace")

    /**
     * Raw stackTrace for a specific thread/fiber (CP6f-2c). The server keys the
     * per-stop frame table by threadId; frame ids are global so they round-trip
     * through scopes/variables unchanged. Structured via [parseStackFrames].
     */
    fun stackTrace(threadId: Int): CompletableFuture<Json> =
        client.sendRequest("stackTrace", Json.obj("threadId" to Json.of(threadId)))

    /** DAP `threads`; structured via [parseThreads]. */
    fun threads(): CompletableFuture<Json> = client.sendRequest("threads", Json.obj())

    /** DAP `scopes` for a frame; structured via [parseScopes]. */
    fun scopes(frameId: Int): CompletableFuture<Json> =
        client.sendRequest("scopes", Json.obj("frameId" to Json.of(frameId)))

    /** DAP `variables` for a scope/var reference; structured via [parseVariables]. */
    fun variables(variablesReference: Int): CompletableFuture<Json> =
        client.sendRequest(
            "variables",
            Json.obj("variablesReference" to Json.of(variablesReference)),
        )

    /**
     * Resolve all locals for a frame: `scopes(frameId)` then `variables(ref)`
     * for each scope, concatenated in scope order. The cajeta server exposes a
     * single "Locals" scope today, but this handles any number. The orchestration
     * lives here (not the platform layer) so it's unit-testable against the fake
     * server and the real binary; the XStackFrame just renders the result.
     */
    fun loadVariables(frameId: Int): CompletableFuture<List<DapVariable>> =
        scopes(frameId).thenCompose { scopesResponse ->
            val scopes = parseScopes(scopesResponse)
            var chain = CompletableFuture.completedFuture(mutableListOf<DapVariable>())
            for (scope in scopes) {
                chain = chain.thenCompose { acc ->
                    variables(scope.variablesReference).thenApply { varsResponse ->
                        // Tag each leaf with its containing scope's ref so the
                        // platform layer can later target setVariable at it.
                        parseVariables(varsResponse).forEach {
                            acc.add(it.copy(containerReference = scope.variablesReference))
                        }
                        acc
                    }
                }
            }
            chain.thenApply { it.toList() }
        }

    /** DAP `disconnect`, then tear down the client transport. */
    fun disconnect(): CompletableFuture<Void?> =
        client.sendRequest("disconnect")
            .handle { _, _ -> null as Void? } // succeed even if the server is already gone
            .whenComplete { _, _ -> client.close() }

    private fun launchArgs(p: LaunchParams): Json = Json.obj(
        "entry-method" to Json.of(p.entryMethod),
        "sourceRoot" to Json.of(p.sourceRoot),
        "stopOnEntry" to Json.of(p.stopOnEntry),
    )

    private fun breakpointArgs(file: String, breakpoints: List<LineBreakpoint>): Json {
        val bps = Json.arr()
        for (bp in breakpoints) {
            val entry = Json.obj("line" to Json.of(bp.line))
            // Only emit a condition when present (whole-file-replace clears the
            // server-side condition for any line sent without one).
            if (bp.condition.isNotBlank()) entry["condition"] = Json.of(bp.condition)
            bps.add(entry)
        }
        return Json.obj(
            "source" to Json.obj("path" to Json.of(file)),
            "breakpoints" to bps,
        )
    }

    companion object {
        /**
         * Map a DAP stackTrace response body to flat frame records. Lines are
         * 1-based as DAP delivers them; path is the source's absolute path
         * (falling back to its name). Pure — unit-tested without a process.
         */
        fun parseStackFrames(stackTraceResponse: Json): List<DapStackFrame> {
            val frames = stackTraceResponse.opt("body")?.opt("stackFrames") ?: return emptyList()
            val out = mutableListOf<DapStackFrame>()
            for (i in 0 until frames.size) {
                val f = frames[i]
                val source = f.opt("source")
                out.add(
                    DapStackFrame(
                        id = f.opt("id")?.asInt() ?: i,
                        name = f.opt("name")?.asString() ?: "<frame>",
                        path = source?.opt("path")?.asString()
                            ?: source?.opt("name")?.asString() ?: "",
                        line = f.opt("line")?.asInt() ?: 0,
                        column = f.opt("column")?.asInt() ?: 1,
                    ),
                )
            }
            return out
        }

        /**
         * Map a DAP `threads` response body to thread records (CP6f-2c). The
         * cajeta server reports the entry/program thread as id 0 ("main") plus
         * one entry per live fiber keyed by its stable dbg id. Pure — unit-tested.
         */
        fun parseThreads(threadsResponse: Json): List<DapThread> {
            val threads = threadsResponse.opt("body")?.opt("threads") ?: return emptyList()
            val out = mutableListOf<DapThread>()
            for (i in 0 until threads.size) {
                val t = threads[i]
                out.add(
                    DapThread(
                        id = t.opt("id")?.asInt() ?: 0,
                        name = t.opt("name")?.asString() ?: "thread ${t.opt("id")?.asInt() ?: 0}",
                    ),
                )
            }
            return out
        }

        /**
         * Map a DAP `scopes` response body to scope records. variablesReference
         * is the handle passed to [variables]. Pure — unit-tested.
         */
        fun parseScopes(scopesResponse: Json): List<DapScope> {
            val scopes = scopesResponse.opt("body")?.opt("scopes") ?: return emptyList()
            val out = mutableListOf<DapScope>()
            for (i in 0 until scopes.size) {
                val s = scopes[i]
                out.add(
                    DapScope(
                        name = s.opt("name")?.asString() ?: "<scope>",
                        variablesReference = s.opt("variablesReference")?.asInt() ?: 0,
                        expensive = s.opt("expensive")?.asBool() ?: false,
                    ),
                )
            }
            return out
        }

        /**
         * Map a DAP `variables` response body to variable records. A non-zero
         * variablesReference marks an expandable value (children fetched by a
         * further [variables] call); the cajeta server emits 0 (leaf) today.
         * Pure — unit-tested.
         */
        fun parseVariables(variablesResponse: Json): List<DapVariable> {
            val vars = variablesResponse.opt("body")?.opt("variables") ?: return emptyList()
            val out = mutableListOf<DapVariable>()
            for (i in 0 until vars.size) {
                val v = vars[i]
                out.add(
                    DapVariable(
                        name = v.opt("name")?.asString() ?: "<var>",
                        value = v.opt("value")?.asString() ?: "",
                        type = v.opt("type")?.asString() ?: "",
                        variablesReference = v.opt("variablesReference")?.asInt() ?: 0,
                    ),
                )
            }
            return out
        }
    }
}

/** A DAP thread/fiber, decoded for the XExecutionStack mapping layer. */
data class DapThread(val id: Int, val name: String)

/** A single DAP stack frame, decoded for the XStackFrame mapping layer. */
data class DapStackFrame(
    val id: Int,
    val name: String,
    val path: String,
    val line: Int,
    val column: Int,
)

/** A DAP scope (e.g. "Locals") with the handle used to fetch its variables. */
data class DapScope(
    val name: String,
    val variablesReference: Int,
    val expensive: Boolean,
)

/** A single DAP variable, decoded for the XValue mapping layer. */
data class DapVariable(
    val name: String,
    val value: String,
    val type: String,
    val variablesReference: Int,
    /** The containing scope's variablesReference; target for setVariable. 0 if unknown. */
    val containerReference: Int = 0,
)
