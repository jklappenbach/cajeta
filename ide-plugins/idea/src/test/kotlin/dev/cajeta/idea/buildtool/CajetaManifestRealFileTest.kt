package dev.cajeta.idea.buildtool

import org.junit.Assert.assertEquals
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File

/**
 * run-config-ergonomics 1.3.1 — acceptance against the REAL manifest in this
 * repo, not a synthetic fixture. Skips (rather than fails) if the file moves, so
 * this stays a signal about parsing and not about repo layout.
 */
class CajetaManifestRealFileTest {

    @Test
    fun readsToolsMcpManifest() {
        val f = File("../../tools/mcp/cajeta.json")
        assumeTrue("tools/mcp/cajeta.json not found at ${f.absolutePath}", f.isFile)

        val b = CajetaManifest.parseBuildSettings(f.readText())
        assertEquals("mcp.Server.main", b.entryMethod)
        assertEquals("src/main/cajeta", b.sourceRoot)
    }
}
