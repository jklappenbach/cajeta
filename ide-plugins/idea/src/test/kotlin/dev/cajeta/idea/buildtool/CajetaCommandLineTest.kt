package dev.cajeta.idea.buildtool

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * W-buildtool unit 4.1.1: the pure `cajeta` argv builder. Deterministic
 * (sorted-key properties/params) so the exact command line is testable and
 * previewable in the Run-with-args dialog.
 */
class CajetaCommandLineTest {

    @Test
    fun bareTaskIsJustTheName() {
        assertEquals(listOf("build"), CajetaCommandLine.runArgv(TaskRunSpec("build")))
    }

    @Test
    fun fullSpecComposesProfileFlavorPropertiesAndParams() {
        val spec = TaskRunSpec(
            task = "test",
            manifestPath = "/p/cajeta.json",
            profile = "integration",
            flavor = "release",
            properties = mapOf("stack-version" to "1.5.0", "arch" to "x64"),
            params = mapOf("filter" to "smoke"),
        )
        assertEquals(
            listOf(
                "test",
                "--manifest=/p/cajeta.json",
                "--profile=integration",
                "--flavor=release",
                "-P", "arch=x64",            // properties sorted by key
                "-P", "stack-version=1.5.0",
                "-p", "filter=smoke",
            ),
            CajetaCommandLine.runArgv(spec),
        )
    }

    @Test
    fun blankSelectionsAreOmitted() {
        val spec = TaskRunSpec(task = "build", profile = "", flavor = "  ", manifestPath = null)
        assertEquals(listOf("build"), CajetaCommandLine.runArgv(spec))
    }

    @Test
    fun discoveryArgvCarriesManifestWhenPresent() {
        assertEquals(listOf("tasks", "--json"), CajetaCommandLine.discoveryArgv(null))
        assertEquals(
            listOf("tasks", "--json", "--manifest=/p/cajeta.json"),
            CajetaCommandLine.discoveryArgv("/p/cajeta.json"),
        )
    }
}
