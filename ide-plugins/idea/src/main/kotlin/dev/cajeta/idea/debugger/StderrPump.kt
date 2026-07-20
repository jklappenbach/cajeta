package dev.cajeta.idea.debugger

import java.io.IOException
import java.io.InputStream

/**
 * Drains a child process's stderr on a daemon thread, forwarding each decoded
 * chunk to [onText]. The DAP server prints large diagnostics there (a failed
 * JIT materialization emits hundreds of KB); if nothing reads the pipe, the
 * OS buffer fills and the server blocks mid-write — the launch then hangs
 * with no response and no visible error. Draining is load-bearing, not
 * cosmetic: every `cajeta dap` child must have its stderr pumped for the
 * whole life of the process.
 */
class StderrPump(
    private val stream: InputStream,
    private val onText: (String) -> Unit,
) {
    fun start(): Thread {
        val thread = Thread({
            try {
                val reader = stream.reader(Charsets.UTF_8)
                val buf = CharArray(8 * 1024)
                while (true) {
                    val n = reader.read(buf)
                    if (n < 0) break
                    if (n > 0) onText(String(buf, 0, n))
                }
            } catch (_: IOException) {
                // Pipe closed with the process; nothing left to drain.
            }
        }, "cajeta-dap-stderr")
        thread.isDaemon = true
        thread.start()
        return thread
    }
}
