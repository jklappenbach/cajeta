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

    /**
     * CP6d end-to-end: parked at the breakpoint, loadVariables drives
     * scopes -> variables against the real server and surfaces the frame's
     * locals (a == 6, b == 7 at `return a * b;`).
     */
    @Test
    fun loadsLocalsAtBreakpoint() {
        val binary = CajetaDapLauncher.locateBinary()
        assumeTrue("cajeta binary not found; set CAJETA_DAP_BIN to run", binary != null)
        binary!!

        val root = Files.createTempDirectory("cajeta-vars-it-").toFile()
        File(root, "demo").apply { mkdirs() }
        File(File(root, "demo"), "Calc.cajeta").writeText(kProg)

        val process = CajetaDapLauncher(binary.absolutePath, CajetaDapLauncher.defaultDllDir()).start()
        val session = CajetaDebugSession(DapClient(DapTransport(process.inputStream, process.outputStream)))

        val stopped = CountDownLatch(1)
        session.onStopped = { stopped.countDown() }
        session.start()

        try {
            session.launch(
                CajetaDebugSession.LaunchParams("demo.Calc.main", root.absolutePath),
                listOf(CajetaDebugSession.LineBreakpoint("Calc.cajeta", 6)),
            ).get(15, TimeUnit.SECONDS)
            assertTrue("breakpoint never hit", stopped.await(30, TimeUnit.SECONDS))

            val st = session.stackTrace().get(10, TimeUnit.SECONDS)
            val topId = st.at("body").at("stackFrames")[0].at("id").asInt()

            val locals = session.loadVariables(topId).get(10, TimeUnit.SECONDS)
            val byName = locals.associateBy { it.name }
            assertTrue("locals missing 'a': ${locals.map { it.name }}", byName.containsKey("a"))
            assertTrue("locals missing 'b': ${locals.map { it.name }}", byName.containsKey("b"))
            assertEquals("6", byName["a"]!!.value)
            assertEquals("7", byName["b"]!!.value)
        } finally {
            session.disconnect()
            process.destroyForcibly()
            root.deleteRecursively()
        }
    }

    /**
     * CP6e end-to-end: parked at the breakpoint, edit a live local through
     * setVariable. The server writes the fiber's stack slot and returns the
     * re-rendered value; a fresh loadVariables read must reflect the change.
     */
    @Test
    fun setLocalUpdatesValue() {
        val binary = CajetaDapLauncher.locateBinary()
        assumeTrue("cajeta binary not found; set CAJETA_DAP_BIN to run", binary != null)
        binary!!

        val root = Files.createTempDirectory("cajeta-setvar-it-").toFile()
        File(root, "demo").apply { mkdirs() }
        File(File(root, "demo"), "Calc.cajeta").writeText(kProg)

        val process = CajetaDapLauncher(binary.absolutePath, CajetaDapLauncher.defaultDllDir()).start()
        val session = CajetaDebugSession(DapClient(DapTransport(process.inputStream, process.outputStream)))

        val stopped = CountDownLatch(1)
        session.onStopped = { stopped.countDown() }
        session.start()

        try {
            session.launch(
                CajetaDebugSession.LaunchParams("demo.Calc.main", root.absolutePath),
                listOf(CajetaDebugSession.LineBreakpoint("Calc.cajeta", 6)),
            ).get(15, TimeUnit.SECONDS)
            assertTrue("breakpoint never hit", stopped.await(30, TimeUnit.SECONDS))

            val st = session.stackTrace().get(10, TimeUnit.SECONDS)
            val topId = st.at("body").at("stackFrames")[0].at("id").asInt()

            val before = session.loadVariables(topId).get(10, TimeUnit.SECONDS)
            val a = before.first { it.name == "a" }
            assertEquals("6", a.value)
            assertTrue("'a' has no container scope ref", a.containerReference != 0)

            // Edit the live local; server re-renders from the written slot.
            val rendered = session.setVariable(a.containerReference, "a", "42").get(10, TimeUnit.SECONDS)
            assertEquals("42", rendered)

            // The change is observable on a fresh read of the locals.
            val after = session.loadVariables(topId).get(10, TimeUnit.SECONDS)
            assertEquals("42", after.first { it.name == "a" }.value)
        } finally {
            session.disconnect()
            process.destroyForcibly()
            root.deleteRecursively()
        }
    }
}
