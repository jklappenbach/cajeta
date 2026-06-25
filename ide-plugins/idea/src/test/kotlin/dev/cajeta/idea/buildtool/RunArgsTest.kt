package dev.cajeta.idea.buildtool

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * W-buildtool unit 12.1.1: the pure Run-with-args model (spec §12). Seeds typed
 * params from their defaults, validates required ones, and composes a
 * [TaskRunSpec] (profile / flavor / `-P` properties / `-p` params) the shared
 * [CajetaCommandLine] turns into argv. No `com.intellij.*`.
 */
class RunArgsTest {

    private val build = CajetaTask(
        name = "build",
        params = listOf(
            TaskParam(name = "flavor", default = "debug", required = false),
            TaskParam(name = "target", default = null, required = true),
        ),
    )

    @Test
    fun initialValuesSeedFromDefaults() {
        val v = RunArgs.initialValues(build)
        assertEquals("debug", v["flavor"])
        assertEquals("", v["target"])   // no default -> blank
    }

    @Test
    fun missingRequiredFlagsBlankRequiredOnly() {
        assertEquals(listOf("target"), RunArgs.missingRequired(build, mapOf("flavor" to "debug", "target" to "")))
        assertTrue(RunArgs.missingRequired(build, mapOf("flavor" to "debug", "target" to "host")).isEmpty())
    }

    @Test
    fun buildSpecOmitsBlankParamsAndCarriesProfileFlavorProps() {
        val spec = RunArgs.buildSpec(
            task = build,
            manifestPath = "/p/cajeta.json",
            profile = "integration",
            flavor = "release",
            properties = mapOf("stack-version" to "1.5.0"),
            paramValues = mapOf("flavor" to "debug", "target" to ""),   // target blank -> omitted
        )
        assertEquals("build", spec.task)
        assertEquals("/p/cajeta.json", spec.manifestPath)
        assertEquals("integration", spec.profile)
        assertEquals("release", spec.flavor)
        assertEquals(mapOf("stack-version" to "1.5.0"), spec.properties)
        assertEquals(mapOf("flavor" to "debug"), spec.params)   // only non-blank params
    }

    @Test
    fun argvReflectsSelections() {
        val spec = RunArgs.buildSpec(
            task = build, manifestPath = null, profile = "integration",
            flavor = "release", properties = emptyMap(),
            paramValues = mapOf("target" to "host"),
        )
        val argv = CajetaCommandLine.runArgv(spec)
        assertEquals(
            listOf("build", "--profile=integration", "--flavor=release", "-p", "target=host"),
            argv,
        )
    }
}
