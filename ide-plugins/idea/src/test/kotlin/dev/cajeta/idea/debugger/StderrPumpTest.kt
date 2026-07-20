package dev.cajeta.idea.debugger

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeFalse
import org.junit.Test
import java.io.ByteArrayInputStream
import java.util.concurrent.TimeUnit

/**
 * The DAP server can print very large diagnostics to stderr (a failed JIT
 * materialization emits hundreds of KB). Nothing read that pipe, so once the
 * OS buffer (~64KB) filled the server blocked mid-write and the launch hung
 * with no response and no visible error — the live symptom was a Debug click
 * that did nothing. The pump exists to make that impossible: it must drain
 * arbitrarily large output and forward it intact.
 */
class StderrPumpTest {

    private fun pumpAll(input: ByteArray): String {
        val sb = StringBuilder()
        val thread = StderrPump(ByteArrayInputStream(input)) { synchronized(sb) { sb.append(it) } }.start()
        thread.join(TimeUnit.SECONDS.toMillis(10))
        assertFalse("pump thread should terminate at EOF", thread.isAlive)
        return synchronized(sb) { sb.toString() }
    }

    @Test
    fun drainsPayloadLargerThanAnyPipeBuffer() {
        val payload = "JIT session error: Symbols not found\n".repeat(30_000) // ~1.1 MB
        assertEquals(payload, pumpAll(payload.toByteArray()))
    }

    @Test
    fun multibyteCharactersSurviveChunkBoundaries() {
        val payload = "sømból-ñøt-føünd→".repeat(20_000)
        assertEquals(payload, pumpAll(payload.toByteArray()))
    }

    @Test
    fun childWritingHugeStderrIsNotDeadlockedByTheDrain() {
        assumeFalse(System.getProperty("os.name").lowercase().contains("win"))
        // Reproduces the live failure shape: the child floods stderr well past
        // the pipe buffer, then reports completion on stdout. Without a drain
        // the write blocks and "done" never arrives.
        val proc = ProcessBuilder(
            "/bin/sh", "-c",
            "i=0; while [ \$i -lt 20000 ]; do echo 'JIT session error: symbols missing' 1>&2; i=\$((i+1)); done; echo done",
        ).start()
        val drained = StringBuilder()
        StderrPump(proc.errorStream) { synchronized(drained) { drained.append(it) } }.start()
        val done = proc.inputStream.bufferedReader().readLine()
        assertTrue("child must run to completion", proc.waitFor(10, TimeUnit.SECONDS))
        assertEquals("done", done)
        assertTrue(synchronized(drained) { drained.length } > 64 * 1024)
    }
}
