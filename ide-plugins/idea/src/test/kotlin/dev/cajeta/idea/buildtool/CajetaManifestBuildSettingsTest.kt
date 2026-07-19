package dev.cajeta.idea.buildtool

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * run-config-ergonomics 1.1 — the `settings.build` read surface and the single
 * entry-method normalization point.
 *
 * Parsing is tested on raw text, with no Project, so it stays pure and cheap.
 * Key spellings are kebab-case, matching what the compiler's Manifest.cpp reads:
 * `entry-method`, `source-root`, and a `binaries` OBJECT keyed by binary name.
 */
class CajetaManifestBuildSettingsTest {

    // 1.1.1
    @Test
    fun readsEntryMethodAndSourceRoot() {
        val json = """
            {
              "settings": {
                "build": {
                  "entry-method": "mcp.Server::main",
                  "source-root": "src/main/cajeta"
                }
              }
            }
        """.trimIndent()
        val b = CajetaManifest.parseBuildSettings(json)
        assertEquals("mcp.Server.main", b.entryMethod)   // normalized on read
        assertEquals("src/main/cajeta", b.sourceRoot)
    }

    // 1.1.2
    @Test
    fun readsEveryBinaryEntryMethodKeyedByName() {
        val json = """
            {
              "settings": {
                "build": {
                  "binaries": {
                    "server": { "entry-method": "mcp.Server::main" },
                    "tool":   { "entry-method": "mcp.Tool.main", "description": "d" }
                  }
                }
              }
            }
        """.trimIndent()
        val b = CajetaManifest.parseBuildSettings(json)
        assertEquals(mapOf("server" to "mcp.Server.main", "tool" to "mcp.Tool.main"),
            b.binaries)
    }

    // 1.1.3 — absent settings.build is valid, not an error.
    @Test
    fun missingBuildBlockYieldsEmptyValues() {
        val b = CajetaManifest.parseBuildSettings("""{"settings": {}}""")
        assertNull(b.entryMethod)
        assertNull(b.sourceRoot)
        assertTrue(b.binaries.isEmpty())
    }

    // 1.1.4 — a broken manifest must never break the editor.
    @Test
    fun malformedJsonYieldsEmptyValuesAndDoesNotThrow() {
        for (bad in listOf("", "{", "not json at all", """{"settings": [1,2}""")) {
            val b = CajetaManifest.parseBuildSettings(bad)
            assertNull("entryMethod for input <$bad>", b.entryMethod)
            assertNull("sourceRoot for input <$bad>", b.sourceRoot)
            assertTrue("binaries for input <$bad>", b.binaries.isEmpty())
        }
    }

    // 1.1.5 — spec 1.4.5 / 2.1.5: one normalization point.
    @Test
    fun normalizationRewritesColonsAndLeavesEverythingElse() {
        assertEquals("mcp.Server.main", CajetaManifest.normalizeEntryMethod("mcp.Server::main"))
        assertEquals("mcp.Server.main", CajetaManifest.normalizeEntryMethod("mcp.Server.main"))
        assertEquals("main", CajetaManifest.normalizeEntryMethod("main"))
        assertEquals("", CajetaManifest.normalizeEntryMethod(""))
    }

    // 1.1.6
    @Test
    fun normalizationIsIdempotent() {
        for (s in listOf("mcp.Server::main", "mcp.Server.main", "main", "")) {
            val once = CajetaManifest.normalizeEntryMethod(s)
            assertEquals("not idempotent for <$s>", once,
                CajetaManifest.normalizeEntryMethod(once))
        }
    }
}
