package dev.cajeta.idea.debugger

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.nio.file.Files
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

/**
 * End-to-end wire test: the real [DapClient] drives a spawned `cajeta dap`
 * process through initialize -> launch -> setBreakpoints -> configurationDone
 * -> (stopped) -> stackTrace -> continue -> (terminated). This proves the
 * framing, seq-correlation, and event dispatch all work over a real pipe
 * against the compiler's own DAP server (CP1-CP4 functionality).
 *
 * The test SKIPS (does not fail) when the `cajeta` binary isn't built or its
 * runtime DLLs aren't locatable, so the suite stays green on a machine that
 * only has the plugin checked out. Point it explicitly with the
 * CAJETA_DAP_BIN env var; on Windows it also prepends the MinGW64 bin dir
 * (CAJETA_DLL_DIR or C:\msys64\mingw64\bin) to the child's PATH so the
 * mingw-linked binary can load libstdc++/libgcc.
 */
class DapClientIntegrationTest {

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

    private fun locateBinary(): File? {
        System.getenv("CAJETA_DAP_BIN")?.let {
            val f = File(it)
            if (f.canExecute()) return f
        }
        val isWindows = System.getProperty("os.name").lowercase().contains("win")
        val exe = if (isWindows) "cajeta.exe" else "cajeta"
        // Test working dir is the plugin module (ide-plugins/idea); the build
        // tree lives two levels up at <repo>/build/src/.
        val moduleDir = File(System.getProperty("user.dir"))
        val candidates = listOf(
            File(moduleDir, "../../build/src/$exe"),
            File(moduleDir, "../../../build/src/$exe"),
        )
        return candidates.map { it.absoluteFile.normalize() }.firstOrNull { it.canExecute() }
    }

    private fun dllDir(): File? {
        System.getenv("CAJETA_DLL_DIR")?.let { return File(it) }
        val def = File("C:\\msys64\\mingw64\\bin")
        return if (def.isDirectory) def else null
    }

    @Test
    fun drivesRealServerToBreakpointAndBack() {
        val binary = locateBinary()
        assumeTrue("cajeta binary not found; set CAJETA_DAP_BIN to run", binary != null)
        binary!!

        // Lay out <root>/demo/Calc.cajeta, matching the package declaration.
        val root = Files.createTempDirectory("cajeta-dap-it-").toFile()
        val pkgDir = File(root, "demo").apply { mkdirs() }
        File(pkgDir, "Calc.cajeta").writeText(kProg)

        val pb = ProcessBuilder(binary.absolutePath, "dap")
        pb.redirectErrorStream(false)
        dllDir()?.let { dir ->
            val env = pb.environment()
            val pathKey = env.keys.firstOrNull { it.equals("PATH", ignoreCase = true) } ?: "PATH"
            env[pathKey] = dir.absolutePath + File.pathSeparator + (env[pathKey] ?: "")
        }

        val process = pb.start()
        val client = DapClient(DapTransport(process.inputStream, process.outputStream))

        val initialized = CountDownLatch(1)
        val stopped = CountDownLatch(1)
        val terminated = CountDownLatch(1)
        val stoppedEvent = AtomicReference<Json>()

        client.onEvent("initialized") { initialized.countDown() }
        client.onEvent("stopped") { ev -> stoppedEvent.set(ev); stopped.countDown() }
        client.onEvent("terminated") { terminated.countDown() }
        client.start()

        try {
            client.sendRequest("initialize").get(10, TimeUnit.SECONDS)
            assertTrue("no 'initialized' event", initialized.await(10, TimeUnit.SECONDS))

            client.sendRequest(
                "launch",
                Json.obj(
                    "entry-method" to Json.of("demo.Calc.main"),
                    "sourceRoot" to Json.of(root.absolutePath),
                ),
            ).get(10, TimeUnit.SECONDS)

            client.sendRequest(
                "setBreakpoints",
                Json.obj(
                    "source" to Json.obj("path" to Json.of("Calc.cajeta")),
                    "breakpoints" to Json.arr(Json.obj("line" to Json.of(6))),
                ),
            ).get(10, TimeUnit.SECONDS)

            client.sendRequest("configurationDone").get(10, TimeUnit.SECONDS)

            assertTrue("breakpoint never hit", stopped.await(30, TimeUnit.SECONDS))
            assertEquals("breakpoint", stoppedEvent.get().at("body").at("reason").asString())

            val st = client.sendRequest("stackTrace").get(10, TimeUnit.SECONDS)
            val frames = st.at("body").at("stackFrames")
            assertTrue("no stack frames", frames.size >= 1)
            assertEquals(6, frames[0].at("line").asInt())

            client.sendRequest("continue").get(10, TimeUnit.SECONDS)
            assertTrue("program never terminated", terminated.await(15, TimeUnit.SECONDS))
        } finally {
            client.close()
            process.destroyForcibly()
            root.deleteRecursively()
        }
    }
}
