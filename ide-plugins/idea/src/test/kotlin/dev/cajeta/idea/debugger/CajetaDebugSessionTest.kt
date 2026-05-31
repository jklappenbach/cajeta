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
    private fun runServer(onCommand: (String, DapTransport) -> Unit = { _, _ -> }) {
        val t = Thread({
            try {
                while (true) {
                    val req = serverTransport.read() ?: break
                    val command = req.at("command").asString()
                    received.add(command)
                    lastRequestByCommand[command] = req
                    serverTransport.write(ok(req))
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
}
