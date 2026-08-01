package dev.cajeta.idea.debugger

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/** Unit tests for the pure command/PATH construction in CajetaDapLauncher. */
class CajetaDapLauncherTest {

    @Test
    fun commandIsBinaryThenDapVerb() {
        val launcher = CajetaDapLauncher("/opt/cajeta/cajeta")
        // The flag rides along so the server's stderr is the structured stream
        // (compiler-jsonl 5.1.2); the DAP protocol is on stdout and unaffected.
        assertEquals(listOf("/opt/cajeta/cajeta", "dap", "--diag-format=json"),
                     launcher.command())
    }

    @Test
    fun childPathPrependsDllDirWhenSet() {
        val launcher = CajetaDapLauncher("cajeta.exe", "C:\\msys64\\mingw64\\bin")
        val result = launcher.childPath("C:\\Windows\\System32")
        assertEquals("C:\\msys64\\mingw64\\bin" + File.pathSeparator + "C:\\Windows\\System32", result)
    }

    @Test
    fun childPathHandlesNullBase() {
        val launcher = CajetaDapLauncher("cajeta.exe", "/usr/lib/cajeta")
        assertEquals("/usr/lib/cajeta", launcher.childPath(null))
    }

    @Test
    fun childPathIsBaseUnchangedWithoutDllDir() {
        val launcher = CajetaDapLauncher("cajeta")
        assertEquals("/usr/bin", launcher.childPath("/usr/bin"))
        assertTrue(launcher.childPath(null).isEmpty())
    }
}
