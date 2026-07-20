package dev.cajeta.idea.debugger

import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.PipedInputStream
import java.io.PipedOutputStream
import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * Deterministic test of the session handshake ordering and event fan-out,
 * driven against an in-memory fake `cajeta dap` connected by pipes. Proves
 * CajetaDebugSession emits initialize -> launch -> setBreakpoints ->
 * configurationDone in order and routes stopped/exited/terminated to its
 * callbacks — without a real process.
 */
class CajetaDebugSessionTest {

    private lateinit var session: CajetaDebugSession
    private lateinit var serverTransport: DapTransport
    private var serverThread: Thread? = null
    private val received = Collections.synchronizedList(mutableListOf<String>())
    private val lastRequestByCommand = java.util.concurrent.ConcurrentHashMap<String, Json>()

    private fun connect() {
        val clientOut = PipedOutputStream()
        val serverIn = PipedInputStream(clientOut, 1 shl 16)
        val serverOut = PipedOutputStream()
        val clientIn = PipedInputStream(serverOut, 1 shl 16)
        serverTransport = DapTransport(serverIn, serverOut)
        session = CajetaDebugSession(DapClient(DapTransport(clientIn, clientOut)))
    }

    private fun ok(req: Json, body: Json = Json.obj()): Json = Json.obj(
        "seq" to Json.of(900),
        "type" to Json.of("response"),
        "request_seq" to Json.of(req.at("seq").asInt()),
        "command" to req.at("command"),
        "success" to Json.of(true),
        "body" to body,
    )

    private fun event(name: String, body: Json = Json.obj()): Json = Json.obj(
        "seq" to Json.of(901),
        "type" to Json.of("event"),
        "event" to Json.of(name),
        "body" to body,
    )

    /**
     * Fake server on a single reader thread: ack every request (recording its
     * command), then run a per-command hook that may emit scripted events.
     */
    private fun runServer(
        respondBody: (String, Json) -> Json = { _, _ -> Json.obj() },
        onCommand: (String, DapTransport) -> Unit = { _, _ -> },
    ) {
        val t = Thread({
            try {
                while (true) {
                    val req = serverTransport.read() ?: break
                    val command = req.at("command").asString()
                    received.add(command)
                    lastRequestByCommand[command] = req
                    serverTransport.write(ok(req, respondBody(command, req)))
                    onCommand(command, serverTransport)
                    if (command == "disconnect") break
                }
            } catch (_: Exception) {
                // pipe closed on teardown
            }
        }, "fake-dap-server")
        t.isDaemon = true
        serverThread = t
        t.start()
    }

    @After
    fun tearDown() {
        if (::session.isInitialized) session.disconnect()
        serverThread?.interrupt()
    }

    @Test
    fun launchSendsHandshakeInOrder() {
        connect()
        runServer()
        session.start()

        session.launch(
            CajetaDebugSession.LaunchParams("demo.Calc.main", "/tmp/root"),
            listOf(CajetaDebugSession.LineBreakpoint("Calc.cajeta", 6)),
        ).get(5, TimeUnit.SECONDS)

        assertEquals(
            listOf("initialize", "launch", "setBreakpoints", "configurationDone"),
            received.toList(),
        )
    }

    @Test
    fun launchOmitsSetBreakpointsWhenNone() {
        connect()
        runServer()
        session.start()

        session.launch(CajetaDebugSession.LaunchParams("demo.Calc.main", "/tmp/root"))
            .get(5, TimeUnit.SECONDS)

        assertEquals(listOf("initialize", "launch", "configurationDone"), received.toList())
    }

    /** 5.1.2 — configured entries reach the server as the standard DAP `env`. */
    @Test
    fun launchCarriesEnvironmentEntries() {
        connect()
        runServer()
        session.start()

        session.launch(
            CajetaDebugSession.LaunchParams(
                "demo.Calc.main", "/tmp/root",
                envVars = linkedMapOf("FOO" to "1", "BAR" to "two"),
            ),
        ).get(5, TimeUnit.SECONDS)

        val env = lastRequestByCommand["launch"]!!.at("arguments").at("env")
        assertEquals("1", env.at("FOO").asString())
        assertEquals("two", env.at("BAR").asString())
    }

    /** 5.1.3 — inheritance is a separate fact from the entries themselves;
     *  the server cannot infer it from an empty map. */
    @Test
    fun launchCarriesInheritFlag() {
        connect()
        runServer()
        session.start()

        session.launch(
            CajetaDebugSession.LaunchParams(
                "demo.Calc.main", "/tmp/root", inheritSystemEnv = false,
            ),
        ).get(5, TimeUnit.SECONDS)

        assertEquals(
            false,
            lastRequestByCommand["launch"]!!.at("arguments")
                .at("inheritSystemEnv").asBool(),
        )
    }

    /**
     * 5.1.4 — the no-regression guard. Unit 5 is inert by design: until the
     * server reads these fields (Unit 6), a default configuration must produce
     * the request it produced before (spec 4.1.6). Asserting on the exact key
     * set is deliberate — an `env: {}` that a future server treated as "empty
     * environment" rather than "unspecified" would silently blank the debuggee's
     * environment, so absence must stay absence on the wire.
     */
    @Test
    fun launchWithNoEnvironmentIsUnchanged() {
        connect()
        runServer()
        session.start()

        session.launch(CajetaDebugSession.LaunchParams("demo.Calc.main", "/tmp/root"))
            .get(5, TimeUnit.SECONDS)

        val args = lastRequestByCommand["launch"]!!.at("arguments") as Json.Obj
        assertEquals(
            setOf("entry-method", "sourceRoot", "stopOnEntry"),
            args.entries.keys,
        )
    }

    @Test
    fun routesStoppedThenExitedThenTerminated() {
        connect()
        val stopped = CountDownLatch(1)
        val terminated = CountDownLatch(1)
        var stopReason = ""
        var exitCode = -1
        session.onStopped = { body -> stopReason = body.at("reason").asString(); stopped.countDown() }
        session.onExited = { code -> exitCode = code }
        session.onTerminated = { terminated.countDown() }

        runServer { command, srv ->
            when (command) {
                // On configurationDone the program "hits a breakpoint".
                "configurationDone" ->
                    srv.write(event("stopped", Json.obj("reason" to Json.of("breakpoint"), "threadId" to Json.of(1))))
                // On continue it runs to exit then termination.
                "continue" -> {
                    srv.write(event("exited", Json.obj("exitCode" to Json.of(42))))
                    srv.write(event("terminated"))
                }
            }
        }
        session.start()
        session.launch(
            CajetaDebugSession.LaunchParams("demo.Calc.main", "/tmp/root"),
            listOf(CajetaDebugSession.LineBreakpoint("Calc.cajeta", 6)),
        ).get(5, TimeUnit.SECONDS)

        assertTrue("no stopped event", stopped.await(5, TimeUnit.SECONDS))
        assertEquals("breakpoint", stopReason)

        session.resume().get(5, TimeUnit.SECONDS)
        assertTrue("no terminated event", terminated.await(5, TimeUnit.SECONDS))
        assertEquals(42, exitCode)
        assertTrue(received.contains("continue"))
    }

    @Test
    fun setBreakpointsSendsSourcePathAndLines() {
        connect()
        runServer()
        session.start()

        session.setBreakpoints("Calc.cajeta", listOf(4, 6)).get(5, TimeUnit.SECONDS)

        val req = lastRequestByCommand["setBreakpoints"]!!
        val args = req.at("arguments")
        assertEquals("Calc.cajeta", args.at("source").at("path").asString())
        val bps = args.at("breakpoints")
        assertEquals(2, bps.size)
        assertEquals(4, bps[0].at("line").asInt())
        assertEquals(6, bps[1].at("line").asInt())
    }

    @Test
    fun setBreakpointsSendsConditionWhenPresent() {
        connect()
        runServer()
        session.start()

        session.setBreakpoints(
            "Calc.cajeta",
            listOf(
                CajetaDebugSession.LineBreakpoint("Calc.cajeta", 4),               // unconditional
                CajetaDebugSession.LineBreakpoint("Calc.cajeta", 6, "a == 6"),     // conditional
            ),
        ).get(5, TimeUnit.SECONDS)

        val bps = lastRequestByCommand["setBreakpoints"]!!.at("arguments").at("breakpoints")
        assertEquals(2, bps.size)
        // Unconditional line carries no "condition" key.
        assertEquals(4, bps[0].at("line").asInt())
        assertEquals(null, bps[0].opt("condition"))
        // Conditional line carries the expression.
        assertEquals(6, bps[1].at("line").asInt())
        assertEquals("a == 6", bps[1].at("condition").asString())
    }

    @Test
    fun parseStackFramesDecodesFramesFromBody() {
        val response = Json.obj(
            "body" to Json.obj(
                "stackFrames" to Json.arr(
                    Json.obj(
                        "id" to Json.of(0),
                        "name" to Json.of("demo.Calc::main"),
                        "line" to Json.of(6),
                        "column" to Json.of(9),
                        "source" to Json.obj(
                            "name" to Json.of("Calc.cajeta"),
                            "path" to Json.of("/tmp/demo/Calc.cajeta"),
                        ),
                    ),
                ),
            ),
        )
        val frames = CajetaDebugSession.parseStackFrames(response)
        assertEquals(1, frames.size)
        assertEquals(0, frames[0].id)
        assertEquals("demo.Calc::main", frames[0].name)
        assertEquals("/tmp/demo/Calc.cajeta", frames[0].path)
        assertEquals(6, frames[0].line)
    }

    @Test
    fun parseStackFramesEmptyWhenNoBody() {
        assertTrue(CajetaDebugSession.parseStackFrames(Json.obj()).isEmpty())
    }

    @Test
    fun stackTraceSendsThreadId() {
        connect()
        runServer()
        session.start()

        session.stackTrace(5).get(5, TimeUnit.SECONDS)

        val req = lastRequestByCommand["stackTrace"]!!
        assertEquals(5, req.at("arguments").at("threadId").asInt())
    }

    @Test
    fun threadsRequestIsSent() {
        connect()
        runServer()
        session.start()

        session.threads().get(5, TimeUnit.SECONDS)

        assertTrue(received.contains("threads"))
    }

    @Test
    fun parseThreadsDecodesThreads() {
        val response = Json.obj(
            "body" to Json.obj(
                "threads" to Json.arr(
                    Json.obj("id" to Json.of(0), "name" to Json.of("main")),
                    Json.obj("id" to Json.of(1), "name" to Json.of("fiber 1")),
                ),
            ),
        )
        val threads = CajetaDebugSession.parseThreads(response)
        assertEquals(2, threads.size)
        assertEquals(0, threads[0].id)
        assertEquals("main", threads[0].name)
        assertEquals(1, threads[1].id)
        assertEquals("fiber 1", threads[1].name)
    }

    @Test
    fun parseThreadsEmptyWhenNoBody() {
        assertTrue(CajetaDebugSession.parseThreads(Json.obj()).isEmpty())
    }

    @Test
    fun setExceptionBreakpointsArmedSendsAllFilter() {
        connect()
        runServer()
        session.start()

        session.setExceptionBreakpoints(true).get(5, TimeUnit.SECONDS)

        val filters = lastRequestByCommand["setExceptionBreakpoints"]!!
            .at("arguments").at("filters")
        assertEquals(1, filters.size)
        assertEquals("all", filters[0].asString())
    }

    @Test
    fun setExceptionBreakpointsDisarmedSendsEmptyFilters() {
        connect()
        runServer()
        session.start()

        session.setExceptionBreakpoints(false).get(5, TimeUnit.SECONDS)

        val filters = lastRequestByCommand["setExceptionBreakpoints"]!!
            .at("arguments").at("filters")
        assertEquals(0, filters.size)
    }

    @Test
    fun scopesSendsFrameId() {
        connect()
        runServer()
        session.start()

        session.scopes(2).get(5, TimeUnit.SECONDS)

        val req = lastRequestByCommand["scopes"]!!
        assertEquals(2, req.at("arguments").at("frameId").asInt())
    }

    @Test
    fun variablesSendsReference() {
        connect()
        runServer()
        session.start()

        session.variables(3).get(5, TimeUnit.SECONDS)

        val req = lastRequestByCommand["variables"]!!
        assertEquals(3, req.at("arguments").at("variablesReference").asInt())
    }

    @Test
    fun loadVariablesChainsScopesThenVariables() {
        connect()
        // Server contract: scopes(frameId) -> Locals with ref=frameId+1;
        // variables(ref) -> that frame's locals. The body is supplied as the
        // request's response (not a separate event), mirroring the real server.
        runServer(respondBody = { command, req ->
            when (command) {
                "scopes" -> {
                    val frameId = req.at("arguments").at("frameId").asInt()
                    Json.obj(
                        "scopes" to Json.arr(
                            Json.obj(
                                "name" to Json.of("Locals"),
                                "variablesReference" to Json.of(frameId + 1),
                                "expensive" to Json.of(false),
                            ),
                        ),
                    )
                }
                "variables" -> Json.obj(
                    "variables" to Json.arr(
                        Json.obj(
                            "name" to Json.of("n"),
                            "value" to Json.of("7"),
                            "type" to Json.of("int"),
                            "variablesReference" to Json.of(0),
                        ),
                        Json.obj(
                            "name" to Json.of("acc"),
                            "value" to Json.of("13"),
                            "type" to Json.of("int"),
                            "variablesReference" to Json.of(0),
                        ),
                    ),
                )
                else -> Json.obj()
            }
        })
        session.start()

        val vars = session.loadVariables(0).get(5, TimeUnit.SECONDS)

        // scopes(0) must precede variables(1) and the ref must be frameId+1.
        assertEquals(1, lastRequestByCommand["variables"]!!.at("arguments").at("variablesReference").asInt())
        assertEquals(listOf("n", "acc"), vars.map { it.name })
        assertEquals("7", vars[0].value)
        assertEquals("int", vars[0].type)
        assertTrue("leaf has no children", vars.all { it.variablesReference == 0 })
    }

    @Test
    fun parseScopesDecodesScopes() {
        val response = Json.obj(
            "body" to Json.obj(
                "scopes" to Json.arr(
                    Json.obj(
                        "name" to Json.of("Locals"),
                        "variablesReference" to Json.of(1),
                        "expensive" to Json.of(false),
                    ),
                ),
            ),
        )
        val scopes = CajetaDebugSession.parseScopes(response)
        assertEquals(1, scopes.size)
        assertEquals("Locals", scopes[0].name)
        assertEquals(1, scopes[0].variablesReference)
        assertEquals(false, scopes[0].expensive)
    }

    @Test
    fun setVariableSendsReferenceNameAndValue() {
        connect()
        runServer(respondBody = { command, _ ->
            if (command == "setVariable") Json.obj("value" to Json.of("42")) else Json.obj()
        })
        session.start()

        session.setVariable(1, "a", "42").get(5, TimeUnit.SECONDS)

        val req = lastRequestByCommand["setVariable"]!!
        val args = req.at("arguments")
        assertEquals(1, args.at("variablesReference").asInt())
        assertEquals("a", args.at("name").asString())
        assertEquals("42", args.at("value").asString())
    }

    @Test
    fun setVariableReturnsRenderedValue() {
        connect()
        runServer(respondBody = { command, _ ->
            if (command == "setVariable") Json.obj("value" to Json.of("99")) else Json.obj()
        })
        session.start()

        val rendered = session.setVariable(1, "a", "99").get(5, TimeUnit.SECONDS)
        assertEquals("99", rendered)
    }

    @Test
    fun loadVariablesTagsContainerReference() {
        connect()
        runServer(respondBody = { command, req ->
            when (command) {
                "scopes" -> {
                    val frameId = req.at("arguments").at("frameId").asInt()
                    Json.obj(
                        "scopes" to Json.arr(
                            Json.obj(
                                "name" to Json.of("Locals"),
                                "variablesReference" to Json.of(frameId + 1),
                                "expensive" to Json.of(false),
                            ),
                        ),
                    )
                }
                "variables" -> Json.obj(
                    "variables" to Json.arr(
                        Json.obj(
                            "name" to Json.of("a"),
                            "value" to Json.of("6"),
                            "type" to Json.of("int"),
                            "variablesReference" to Json.of(0),
                        ),
                    ),
                )
                else -> Json.obj()
            }
        })
        session.start()

        val vars = session.loadVariables(0).get(5, TimeUnit.SECONDS)
        // The scope ref for frame 0 is 1; every leaf must carry it so the UI
        // can target setVariable at the containing scope.
        assertEquals(1, vars[0].containerReference)
    }

    @Test
    fun parseVariablesDecodesVariables() {
        val response = Json.obj(
            "body" to Json.obj(
                "variables" to Json.arr(
                    Json.obj(
                        "name" to Json.of("acc"),
                        "value" to Json.of("13"),
                        "type" to Json.of("int"),
                        "variablesReference" to Json.of(0),
                    ),
                ),
            ),
        )
        val vars = CajetaDebugSession.parseVariables(response)
        assertEquals(1, vars.size)
        assertEquals("acc", vars[0].name)
        assertEquals("13", vars[0].value)
        assertEquals("int", vars[0].type)
        assertEquals(0, vars[0].variablesReference)
    }
}
