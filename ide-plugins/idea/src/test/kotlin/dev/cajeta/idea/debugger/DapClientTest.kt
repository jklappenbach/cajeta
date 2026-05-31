package dev.cajeta.idea.debugger

import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.PipedInputStream
import java.io.PipedOutputStream
import java.util.concurrent.CountDownLatch
import java.util.concurrent.ExecutionException
import java.util.concurrent.TimeUnit

/**
 * Unit tests for the DAP client's seq-correlation and event dispatch, driven
 * against an in-memory fake server connected by piped streams. No process,
 * no IntelliJ platform.
 */
class DapClientTest {

    private lateinit var client: DapClient
    private lateinit var serverTransport: DapTransport
    private var serverThread: Thread? = null

    private fun connect(): DapTransport {
        // client -> server channel
        val clientOut = PipedOutputStream()
        val serverIn = PipedInputStream(clientOut, 1 shl 16)
        // server -> client channel
        val serverOut = PipedOutputStream()
        val clientIn = PipedInputStream(serverOut, 1 shl 16)

        serverTransport = DapTransport(serverIn, serverOut)
        client = DapClient(DapTransport(clientIn, clientOut))
        client.start()
        return serverTransport
    }

    /** Build a success response echoing the request's seq + command. */
    private fun response(req: Json, body: Json = Json.obj(), success: Boolean = true, message: String? = null): Json {
        val r = Json.obj(
            "seq" to Json.of(900),
            "type" to Json.of("response"),
            "request_seq" to Json.of(req.at("seq").asInt()),
            "command" to req.at("command"),
            "success" to Json.of(success),
            "body" to body,
        )
        if (message != null) r["message"] = Json.of(message)
        return r
    }

    private fun runServer(block: (DapTransport) -> Unit) {
        val t = Thread({ block(serverTransport) }, "fake-dap-server")
        t.isDaemon = true
        serverThread = t
        t.start()
    }

    @After
    fun tearDown() {
        client.close()
        serverThread?.interrupt()
    }

    @Test
    fun correlatesResponsesToRequestsEvenOutOfOrder() {
        connect()
        runServer { srv ->
            val r1 = srv.read()!! // "first"
            val r2 = srv.read()!! // "second"
            // Reply in REVERSE order; correlation must still route correctly.
            srv.write(response(r2, Json.obj("which" to Json.of("second"))))
            srv.write(response(r1, Json.obj("which" to Json.of("first"))))
        }

        val f1 = client.sendRequest("first")
        val f2 = client.sendRequest("second")

        val resp1 = f1.get(5, TimeUnit.SECONDS)
        val resp2 = f2.get(5, TimeUnit.SECONDS)
        assertEquals("first", resp1.at("command").asString())
        assertEquals("first", resp1.at("body").at("which").asString())
        assertEquals("second", resp2.at("command").asString())
        assertEquals("second", resp2.at("body").at("which").asString())
    }

    @Test
    fun dispatchesEventsToRegisteredHandlers() {
        connect()
        val latch = CountDownLatch(1)
        var stoppedThread = -1
        client.onEvent("stopped") { ev ->
            stoppedThread = ev.at("body").at("threadId").asInt()
            latch.countDown()
        }

        runServer { srv ->
            val req = srv.read()!! // initialize
            srv.write(response(req))
            srv.write(
                Json.obj(
                    "seq" to Json.of(901),
                    "type" to Json.of("event"),
                    "event" to Json.of("stopped"),
                    "body" to Json.obj("reason" to Json.of("breakpoint"), "threadId" to Json.of(1)),
                ),
            )
        }

        client.sendRequest("initialize").get(5, TimeUnit.SECONDS)
        assertTrue("stopped event not delivered", latch.await(5, TimeUnit.SECONDS))
        assertEquals(1, stoppedThread)
    }

    @Test
    fun failureResponseCompletesExceptionally() {
        connect()
        runServer { srv ->
            val req = srv.read()!!
            srv.write(
                response(
                    req,
                    body = Json.obj("value" to Json.of("")),
                    success = false,
                    message = "cannot set object variable",
                ),
            )
        }

        val future = client.sendRequest("setVariable")
        try {
            future.get(5, TimeUnit.SECONDS)
            fail("expected the request to fail")
        } catch (e: ExecutionException) {
            val cause = e.cause
            assertTrue(cause is DapRequestException)
            cause as DapRequestException
            assertEquals("setVariable", cause.command)
            assertTrue(cause.message!!.contains("cannot set object variable"))
        }
    }

    @Test
    fun closeFailsInFlightRequests() {
        connect()
        // No server replies; the request is in-flight when we close.
        val future = client.sendRequest("stackTrace")
        client.close()
        try {
            future.get(5, TimeUnit.SECONDS)
            fail("expected the in-flight request to fail on close")
        } catch (e: ExecutionException) {
            assertTrue(e.cause is IllegalStateException)
        }
    }
}
