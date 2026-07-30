package dev.cajeta.idea.buildtool

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertFalse
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

    /**
     * 2.3.1, resolver half — the value the editor will prefill for the real
     * tools/mcp project. That the editor RENDERS it is a live-IDE check; this
     * pins that the right path is computed from the real manifest and tree.
     */
    @Test
    fun defaultSourceRootForToolsMcpComesFromItsManifest() {
        val base = File("../../tools/mcp")
        assumeTrue("tools/mcp not found at ${base.absolutePath}",
            File(base, "cajeta.json").isFile)

        val manifest = CajetaManifest.parseBuildSettings(File(base, "cajeta.json").readText())
        val resolved = CajetaRoots.defaultSourceRoot(base.path, manifest.sourceRoot)

        assertEquals(File(base, "src/main/cajeta").path, resolved)
        assertTrue("resolved source root should exist on disk: $resolved",
            File(resolved).isDirectory)
    }

    /**
     * The manifest that exposed the strict-JSON bug: samples/tour/cajeta.json
     * opens with a block of `//` comments, so the plugin read NOTHING from it
     * and the run configuration lost every manifest default.
     */
    @Test
    fun readsTheCommentedTourManifest() {
        val f = File("../../samples/tour/cajeta.json")
        assumeTrue("samples/tour/cajeta.json not found at ${f.absolutePath}", f.isFile)

        val b = CajetaManifest.parseBuildSettings(f.readText())
        assertNotNull("entry-method should be read despite // comments", b.entryMethod)
        assertNotNull("source-root should be read despite // comments", b.sourceRoot)
        assertFalse("entry method must be normalized", b.entryMethod!!.contains("::"))
    }
    /**
     * Julian 2026-07-30: opening cajeta-logging samples/tour, the run-config
     * dialog offered no main even though its manifest DECLARES one. Pin the
     * declared-candidate path against that exact manifest.
     */
    @Test
    fun readsCajetaLoggingTourManifest() {
        val f = File(System.getProperty("user.home"),
            "code/cpp/cajeta-logging/samples/tour/cajeta.json")
        assumeTrue("tour manifest not found at ${f.absolutePath}", f.isFile)

        val b = CajetaManifest.parseBuildSettings(f.readText())
        assertEquals("tour.LoggingTour.main", b.entryMethod)
    }
}
