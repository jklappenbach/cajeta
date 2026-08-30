package dev.cajeta.idea.lint

import dev.cajeta.idea.debugger.Json

/**
 * The injectable IO handle to one warm lint server: line-framed stdin/stdout
 * plus liveness and teardown. Bound to a real `cajeta --lint-server` subprocess
 * by [LintServerClient]; faked in tests so the protocol is the only thing under
 * test — the same split as [dev.cajeta.idea.debugger.ResidentDapCore].
 */
interface LintServerTransport {
    fun writeLine(line: String)
    fun readLine(): String?   // a line from server stdout; null at EOF
    fun isAlive(): Boolean
    fun close()
}

/** One warm-lint request: the staged buffer, its on-disk twin, the xref toggle
 *  (lint-server-spec §2). Source root and classpath are the server's start-time
 *  context, not per-request. */
data class LintServerRequest(
    val file: String,
    val shadow: String?,
    val emitXref: Boolean,
)

/**
 * The pure lint-server protocol (lint-server-spec §2). On first use it spawns a
 * server and reads its ready handshake; each [lint] then sends one NDJSON
 * request and collects that response's payload — the diagnostic/xref lines,
 * byte-identical to one-shot lint — up to the `done` marker. Everything that can
 * go wrong resolves to a [Result] the caller turns into a one-shot fallback:
 *
 *  - an unknown protocol major latches [Result.Unsupported] and never sends a
 *    request (§2.6),
 *  - a dead server / desynced id / mid-stream error yields [Result.Failed] and
 *    tears the server down; the next call respawns, but not before a backoff so
 *    a crash-looping binary doesn't get hammered (§2.5).
 *
 * Process creation and the clock are injected, so the whole thing is
 * unit-testable with a fake transport.
 */
class LintServerCore(
    private val spawn: () -> LintServerTransport,
    private val backoffNanos: Long = 2_000_000_000L,   // 2s between respawns
    private val now: () -> Long = System::nanoTime,
    /**
     * The content identity of the binary this core is configured to spawn, or
     * null when it cannot be read (§2.8). A core built without one never judges
     * a server stale, which is exactly how this behaved before Unit 5.
     */
    private val identityOf: (() -> String?)? = null,
    /** A live server was found to be running replaced code; it is being torn
     *  down and respawned. (reported identity, identity on disk now) */
    private val onStale: (String, String?) -> Unit = { _, _ -> },
    /** A FRESH server disagreed with the binary we believe we are calling, so
     *  the comparison is unusable and has been switched off for this core. */
    private val onCheckDisabled: (String, String?) -> Unit = { _, _ -> },
) {
    sealed class Result {
        /** The response payload — the lint's diagnostic/xref lines verbatim. */
        data class Payload(val text: String) : Result()
        /** The server speaks a protocol major we can't read; use one-shot. */
        object Unsupported : Result()
        /** The server was unreachable/dead this time; use one-shot, retry later. */
        object Failed : Result()
    }

    companion object {
        /** The protocol major this client can read (lint-server-spec §2). */
        const val SUPPORTED_MAJOR = 1
    }

    private var transport: LintServerTransport? = null
    private var ready = false
    private var supported = true
    private var unsupportedLatched = false
    private var nextId = 0
    private var lastFailNanos: Long? = null
    // The binary identity the live server reported at its handshake; null when
    // it reported none (a server older than §2.8), which cannot be judged.
    private var serverBinary: String? = null
    // Latched when a fresh server's reported identity and ours disagree: the
    // two are not reading the same bytes, so comparing them again would restart
    // on every edit.
    private var identityCheckDisabled = false

    @Synchronized
    fun lint(req: LintServerRequest): Result {
        if (unsupportedLatched) return Result.Unsupported
        val t = ensureReady() ?: return Result.Failed
        if (!supported) {
            // The handshake reported a major we can't read: latch it, drop the
            // server, and never send a request against it (§2.6).
            unsupportedLatched = true
            closeTransport()
            return Result.Unsupported
        }

        val id = ++nextId
        try {
            t.writeLine(requestJson(id, req))
        } catch (_: Exception) {
            return fail()
        }

        // Collect payload lines until this request's `done`. Control records
        // (done/error/server) are recognized by kind; every other line is
        // payload — diagnostics and xref records the caller parses exactly as
        // it parses one-shot output.
        val payload = StringBuilder()
        while (true) {
            val line = try {
                t.readLine()
            } catch (_: Exception) {
                return fail()
            } ?: return fail()   // EOF before done: the server died mid-response.

            when (controlKind(line)) {
                "done" -> {
                    val doneId = controlId(line)
                    if (doneId != null && doneId != id) return fail()  // desync
                    return Result.Payload(payload.toString())
                }
                "error" -> return fail()         // server-reported error → fall back
                "server" -> return fail()        // stray ready mid-stream → desync
                else -> payload.append(line).append('\n')
            }
        }
    }

    /** Best-effort clean shutdown (stdin `shutdown` record, then close). */
    @Synchronized
    fun shutdown() {
        transport?.let { runCatching { it.writeLine("{\"kind\":\"shutdown\"}") } }
        closeTransport()
    }

    /** True once a server is spawned, alive, and past its ready handshake. */
    @Synchronized
    fun isReady(): Boolean = transport?.isAlive() == true && ready && supported

    private fun ensureReady(): LintServerTransport? {
        val live = transport
        if (live != null && live.isAlive() && ready) {
            val reported = serverBinary
            if (identityOf == null || identityCheckDisabled || reported == null) return live
            val current = currentIdentity()
            if (current == reported) return live
            // The compiler was rebuilt (or went missing) under a live daemon.
            // Serving from it means a fix that silently does not take effect —
            // and, on the xref path, a stale shard overwriting a good one. A
            // stale server is not a failure, so no backoff applies: tear it
            // down and fall through to a fresh spawn.
            onStale(reported, current)
            closeTransport()
        }

        // Respawn — but not immediately after a failure (§2.5 backoff), so a
        // binary that crashes on start doesn't get relaunched on every edit.
        val lf = lastFailNanos
        if (lf != null && now() - lf < backoffNanos) return null

        closeTransport()
        val t = try {
            spawn()
        } catch (_: Exception) {
            recordFail(); return null
        }
        transport = t
        ready = false
        supported = true

        val line = try {
            t.readLine()
        } catch (_: Exception) {
            recordFail(); return null
        } ?: run { recordFail(); return null }

        val root = parseObj(line)
        val kind = (root?.opt("kind") as? Json.Str)?.value
        val state = (root?.opt("state") as? Json.Str)?.value
        if (root == null || kind != "server" || state != "ready") {
            recordFail(); return null
        }
        val proto = root.opt("proto") as? Json.Obj
        val major = (proto?.opt("major") as? Json.Num)?.value?.toInt()
        supported = major == SUPPORTED_MAJOR
        serverBinary =
            ((root.opt("binary") as? Json.Obj)?.opt("id") as? Json.Str)?.value
        ready = true
        lastFailNanos = null

        // A server we JUST spawned is by definition running the binary we
        // called. If its identity still disagrees with ours, the two are not
        // looking at the same bytes — a wrapper, a second install, an
        // unreadable file — and every later comparison would restart a healthy
        // daemon. Say so once and stop checking.
        val reported = serverBinary
        if (identityOf != null && reported != null && !identityCheckDisabled) {
            val cur = currentIdentity()
            if (cur != reported) {
                identityCheckDisabled = true
                onCheckDisabled(reported, cur)
            }
        }
        return t
    }

    /** Our reading of the configured binary. A check that throws is a broken
     *  instrument, not a verdict, so it reads as "cannot tell". */
    private fun currentIdentity(): String? {
        val f = identityOf ?: return null
        return try { f() } catch (_: Exception) { null }
    }

    private fun fail(): Result {
        recordFail()
        closeTransport()
        return Result.Failed
    }

    private fun recordFail() { lastFailNanos = now() }

    private fun closeTransport() {
        transport?.let { runCatching { it.close() } }
        transport = null
        ready = false
        serverBinary = null
    }

    private fun controlKind(line: String): String? {
        val root = parseObj(line) ?: return null
        val k = (root.opt("kind") as? Json.Str)?.value
        return if (k == "done" || k == "error" || k == "server") k else null
    }

    private fun controlId(line: String): Int? =
        (parseObj(line)?.opt("id") as? Json.Num)?.value?.toInt()

    private fun parseObj(line: String): Json.Obj? {
        val t = line.trim()
        if (t.isEmpty() || t[0] != '{') return null
        return try {
            Json.parse(t) as? Json.Obj
        } catch (_: Exception) {
            null
        }
    }

    private fun requestJson(id: Int, req: LintServerRequest): String {
        val sb = StringBuilder()
        sb.append("{\"kind\":\"lint\",\"id\":").append(id)
        sb.append(",\"file\":\"").append(esc(req.file)).append('"')
        if (req.shadow != null)
            sb.append(",\"shadow\":\"").append(esc(req.shadow)).append('"')
        sb.append(",\"emitXref\":").append(req.emitXref)
        sb.append('}')
        return sb.toString()
    }

    private fun esc(s: String): String {
        val b = StringBuilder()
        for (c in s) when (c) {
            '\\' -> b.append("\\\\")
            '"' -> b.append("\\\"")
            '\n' -> b.append("\\n")
            '\r' -> b.append("\\r")
            '\t' -> b.append("\\t")
            else -> b.append(c)
        }
        return b.toString()
    }
}
