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
 * Stack/scopes/variables mapping and real breakpoint sync arrive in CP6c/d;
 * here [stackTrace] is a raw passthrough and [launch] accepts a static
 * breakpoint list so the skeleton can prove a stop end-to-end.
 */
class CajetaDebugSession(private val client: DapClient) {

    data class LaunchParams(
        val entryMethod: String,
        val sourceRoot: String,
        val stopOnEntry: Boolean = false,
    )

    data class LineBreakpoint(val file: String, val line: Int)

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
        for ((file, lines) in breakpoints.groupBy { it.file }) {
            chain = chain.thenCompose {
                client.sendRequest("setBreakpoints", breakpointArgs(file, lines.map { it.line }))
            }
        }
        return chain
            .thenCompose { client.sendRequest("configurationDone") }
            .thenApply { null }
    }

    /** DAP `continue` — resume the parked program. */
    fun resume(): CompletableFuture<Json> = client.sendRequest("continue")

    /**
     * Replace the breakpoints for one source file (DAP setBreakpoints is
     * whole-file-replace). Used for live add/remove once the session is running;
     * the initial set goes through [launch].
     */
    fun setBreakpoints(file: String, lines: List<Int>): CompletableFuture<Json> =
        client.sendRequest("setBreakpoints", breakpointArgs(file, lines))

    /** Raw stackTrace response; structured frames via [parseStackFrames]. */
    fun stackTrace(): CompletableFuture<Json> = client.sendRequest("stackTrace")

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

    private fun breakpointArgs(file: String, lines: List<Int>): Json {
        val bps = Json.arr()
        for (line in lines) bps.add(Json.obj("line" to Json.of(line)))
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
    }
}

/** A single DAP stack frame, decoded for the XStackFrame mapping layer. */
data class DapStackFrame(
    val id: Int,
    val name: String,
    val path: String,
    val line: Int,
    val column: Int,
)
