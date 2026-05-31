package dev.cajeta.idea.debugger

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.nio.file.Files
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * End-to-end CP6b test: CajetaDapLauncher spawns the real `cajeta dap`
 * process and CajetaDebugSession drives the full launch handshake, hits a
 * line breakpoint, reads the stack, resumes, and runs to termination — the
 * exact path the XDebugProcess delegates. Skips cleanly when the binary or
 * its runtime DLLs aren't present.
 */
class CajetaDebugSessionIntegrationTest {

    private val kProg = """
        package demo;
        public class Calc {
            public static int32 main() {
                int32 a = 6;
                int32 b = 7;
                return a * b;
            }
        }
    """.trimIndent() + "\n"

    @Test
    fun launchesHitsBreakpointAndResumesToTermination() {
        val binary = CajetaDapLauncher.locateBinary()
        assumeTrue("cajeta binary not found; set CAJETA_DAP_BIN to run", binary != null)
        binary!!

        val root = Files.createTempDirectory("cajeta-session-it-").toFile()
        File(root, "demo").apply { mkdirs() }
        File(File(root, "demo"), "Calc.cajeta").writeText(kProg)

        val process = CajetaDapLauncher(binary.absolutePath, CajetaDapLauncher.defaultDllDir()).start()
        val session = CajetaDebugSession(DapClient(DapTransport(process.inputStream, process.outputStream)))

        val stopped = CountDownLatch(1)
        val terminated = CountDownLatch(1)
        var exitCode = -1
        var stopReason = ""
        session.onStopped = { body -> stopReason = body.opt("reason")?.asString() ?: ""; stopped.countDown() }
        session.onExited = { code -> exitCode = code }
        session.onTerminated = { terminated.countDown() }
        session.start()

        try {
            session.launch(
                CajetaDebugSession.LaunchParams("demo.Calc.main", root.absolutePath),
                listOf(CajetaDebugSession.LineBreakpoint("Calc.cajeta", 6)),
            ).get(15, TimeUnit.SECONDS)

            assertTrue("breakpoint never hit", stopped.await(30, TimeUnit.SECONDS))
            assertEquals("breakpoint", stopReason)

            val st = session.stackTrace().get(10, TimeUnit.SECONDS)
            val frames = st.at("body").at("stackFrames")
            assertTrue("no stack frames", frames.size >= 1)
            assertEquals(6, frames[0].at("line").asInt())

            session.resume().get(10, TimeUnit.SECONDS)
            assertTrue("program never terminated", terminated.await(15, TimeUnit.SECONDS))
            assertEquals(42, exitCode)
        } finally {
            session.disconnect()
            process.destroyForcibly()
            root.deleteRecursively()
        }
    }
}
