package dev.cajeta.idea.buildtool

import com.intellij.util.xmlb.XmlSerializerUtil
import dev.cajeta.idea.settings.CajetaSettings
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * W-buildtool unit 2: the new build-tool settings fields (spec §14) default
 * sanely and round-trip through the persisted State, and the buildToolPath
 * validator flags bad paths (§14.2.1). Pure — State is a data class and the
 * validator is a filesystem stat, so no platform fixture.
 */
class BuildToolSettingsTest {

    @Test
    fun newFieldsHaveSpecDefaults() {
        val s = CajetaSettings.State()
        assertEquals("cajeta", s.buildToolPath)             // resolved on PATH
        assertEquals(CajetaSettings.AUTO_RELOAD_PROMPT, s.buildAutoReload)
        assertEquals("", s.defaultProfile)
        assertEquals("", s.defaultFlavor)
        assertTrue(s.jsonlDefaultStructured)                // §14.2.2 structured default
        assertEquals("", s.jsonlDefaultLevel)               // all levels
        assertTrue(s.buildTasksInBuildWindow)               // build-toolwindow §6.1 default on
    }

    @Test
    fun buildTasksInBuildWindowRoundTrips() {
        val src = CajetaSettings.State(buildTasksInBuildWindow = false)
        val dst = CajetaSettings.State()
        XmlSerializerUtil.copyBean(src, dst)
        assertEquals(false, dst.buildTasksInBuildWindow)
    }

    @Test
    fun stateRoundTripsThroughCopyBean() {
        val src = CajetaSettings.State(
            buildToolPath = "/opt/cajeta/bin/cajeta",
            buildAutoReload = CajetaSettings.AUTO_RELOAD_ALWAYS,
            defaultProfile = "ci",
            defaultFlavor = "release",
            jsonlDefaultStructured = false,
            jsonlDefaultLevel = "warn",
        )
        val dst = CajetaSettings.State()
        XmlSerializerUtil.copyBean(src, dst)   // the same path loadState() uses

        assertEquals("/opt/cajeta/bin/cajeta", dst.buildToolPath)
        assertEquals(CajetaSettings.AUTO_RELOAD_ALWAYS, dst.buildAutoReload)
        assertEquals("ci", dst.defaultProfile)
        assertEquals("release", dst.defaultFlavor)
        assertEquals(false, dst.jsonlDefaultStructured)
        assertEquals("warn", dst.jsonlDefaultLevel)
    }

    @Test
    fun validatorAcceptsBareNameAsPathLookup() {
        val r = BuildToolPathValidator.validate("cajeta")
        assertTrue(r is BuildToolPathValidator.Result.Ok)
        assertNull(BuildToolPathValidator.problem("cajeta"))
    }

    @Test
    fun validatorFlagsEmptyAndMissingAndNonExecutable() {
        assertTrue(BuildToolPathValidator.validate("   ") is BuildToolPathValidator.Result.Invalid)
        assertTrue(
            BuildToolPathValidator.validate("/no/such/cajeta-binary-xyz")
                is BuildToolPathValidator.Result.Invalid,
        )
        // A directory is not a runnable build tool.
        val tmpDir = System.getProperty("java.io.tmpdir")
        assertTrue(
            BuildToolPathValidator.validate(tmpDir) is BuildToolPathValidator.Result.Invalid,
        )
    }

    @Test
    fun validatorAcceptsAnExistingExecutable() {
        // /bin/sh is a stable executable on the CI host; stand-in for a real
        // explicit build-tool path.
        val sh = File("/bin/sh")
        if (sh.exists() && sh.canExecute()) {
            assertTrue(
                BuildToolPathValidator.validate(sh.path) is BuildToolPathValidator.Result.Ok,
            )
            assertNull(BuildToolPathValidator.problem(sh.path))
        }
    }
}
