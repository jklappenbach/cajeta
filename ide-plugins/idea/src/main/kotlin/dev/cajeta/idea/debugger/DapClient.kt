package dev.cajeta.idea.debugger

import java.util.concurrent.CompletableFuture
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicInteger

/**
 * A request fails when the server replies with `success:false`. Carries the
 * command, the server's human-readable message, and the raw response so
 * callers can inspect a `body` (e.g. `setVariable`'s rejection text).
 */
class DapRequestException(
    val command: String,
    message: String,
    val response: Json,
) : RuntimeException("DAP request '$command' failed: $message")

/**
 * The DAP wire loop on top of [DapTransport]: assigns monotonically
 * increasing `seq` ids, correlates each response back to its request by
 * `request_seq`, and fans events out to registered handlers. A single daemon
 * reader thread owns all inbound reads; outbound writes go straight through
 * the transport (which serializes them), so [sendRequest] is safe to call
 * from any thread.
 *
 * This class is plain JVM code with no IntelliJ-platform dependency, so it is
 * exercised end-to-end by fast JUnit tests against in-memory pipes and against
 * a real `cajeta dap` process. The XDebugProcess layer (CP6b+) drives it.
 */
class DapClient(private val transport: DapTransport) {

    private val seq = AtomicInteger(1)
    private val pending = ConcurrentHashMap<Int, CompletableFuture<Json>>()
    private val eventHandlers = ConcurrentHashMap<String, CopyOnWriteArrayList<(Json) -> Unit>>()

    @Volatile private var readerThread: Thread? = null
    @Volatile private var closed = false

    /** Invoked once when the reader loop ends (clean EOF or error). */
    @Volatile var onClosed: (() -> Unit)? = null

    /** Start the background reader. Idempotent. */
    fun start() {
        if (readerThread != null) return
        val t = Thread({ readLoop() }, "cajeta-dap-reader")
        t.isDaemon = true
        readerThread = t
        t.start()
    }

    /** Register a handler for a DAP event (e.g. "stopped", "terminated"). */
    fun onEvent(event: String, handler: (Json) -> Unit) {
        eventHandlers.computeIfAbsent(event) { CopyOnWriteArrayList() }.add(handler)
    }

    /**
     * Send a request and return a future for its response. The future
     * completes with the full response message on `success:true`, or completes
     * exceptionally with [DapRequestException] on `success:false`.
     */
    fun sendRequest(command: String, arguments: Json = Json.obj()): CompletableFuture<Json> {
        val s = seq.getAndIncrement()
        val req = Json.obj(
            "seq" to Json.of(s),
            "type" to Json.of("request"),
            "command" to Json.of(command),
            "arguments" to arguments,
        )
        val future = CompletableFuture<Json>()
        pending[s] = future
        try {
            transport.write(req)
        } catch (e: Exception) {
            pending.remove(s)
            future.completeExceptionally(e)
        }
        return future
    }

    /** Stop the reader and fail any in-flight requests. */
    fun close() {
        if (closed) return
        closed = true
        readerThread?.interrupt()
        failAllPending(IllegalStateException("DAP client closed"))
    }

    private fun readLoop() {
        try {
            while (!closed) {
                val msg = transport.read() ?: break
                dispatch(msg)
            }
        } catch (e: Exception) {
            if (!closed) failAllPending(e)
        } finally {
            failAllPending(IllegalStateException("DAP connection closed"))
            onClosed?.invoke()
        }
    }

    private fun dispatch(msg: Json) {
        when (msg.opt("type")?.asString()) {
            "response" -> {
                val requestSeq = msg.opt("request_seq")?.asInt() ?: return
                val future = pending.remove(requestSeq) ?: return
                val success = msg.opt("success")?.asBool() ?: false
                if (success) {
                    future.complete(msg)
                } else {
                    future.completeExceptionally(
                        DapRequestException(
                            command = msg.opt("command")?.asString() ?: "?",
                            message = msg.opt("message")?.asString() ?: "request failed",
                            response = msg,
                        ),
                    )
                }
            }
            "event" -> {
                val name = msg.opt("event")?.asString() ?: return
                eventHandlers[name]?.forEach { it(msg) }
            }
            // "request" (reverse requests) not used by cajeta dap; ignore.
        }
    }

    private fun failAllPending(cause: Throwable) {
        val it = pending.entries.iterator()
        while (it.hasNext()) {
            val (_, future) = it.next()
            it.remove()
            future.completeExceptionally(cause)
        }
    }
}
