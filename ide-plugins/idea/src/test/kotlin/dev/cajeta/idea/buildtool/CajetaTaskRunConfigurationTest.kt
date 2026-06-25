package dev.cajeta.idea.buildtool

import com.intellij.execution.configurations.ConfigurationTypeUtil
import com.intellij.testFramework.fixtures.BasePlatformTestCase

/**
 * W-buildtool unit 6.1.1: the task run-configuration type is registered, its
 * options round-trip through the typed accessors, and the command line it
 * derives reuses the shared [CajetaCommandLine] (§4.1.1). Runs headless under
 * the IntelliJ test fixture; the streaming-console execution is a runIde smoke.
 */
class CajetaTaskRunConfigurationTest : BasePlatformTestCase() {

    fun testConfigurationTypeIsRegistered() {
        val type = ConfigurationTypeUtil.findConfigurationType(CajetaTaskConfigurationType::class.java)
        assertNotNull("CajetaTaskConfigurationType not registered", type)
        assertEquals(1, type!!.configurationFactories.size)
        assertEquals("Cajeta Task", type.displayName)
    }

    fun testOptionsRoundTripAndArgvDerivation() {
        val type = ConfigurationTypeUtil.findConfigurationType(CajetaTaskConfigurationType::class.java)!!
        val config = type.configurationFactories[0]
            .createTemplateConfiguration(project) as CajetaTaskRunConfiguration

        config.task = "test"
        config.manifestPath = "/p/cajeta.json"
        config.profile = "integration"
        config.flavor = "release"
        config.propertiesText = "arch=x64\nstack-version=1.5.0"
        config.paramsText = "filter=smoke"

        // Round-trip through the typed accessors (persisted options).
        assertEquals("test", config.task)
        assertEquals("/p/cajeta.json", config.manifestPath)
        assertEquals("integration", config.profile)
        assertEquals("release", config.flavor)
        assertEquals("arch=x64\nstack-version=1.5.0", config.propertiesText)
        assertEquals("filter=smoke", config.paramsText)

        // The derived argv reuses the shared pure builder (§4.1.1).
        assertEquals(
            listOf(
                "test", "--manifest=/p/cajeta.json", "--profile=integration", "--flavor=release",
                "-P", "arch=x64", "-P", "stack-version=1.5.0", "-p", "filter=smoke",
            ),
            CajetaCommandLine.runArgv(config.toRunSpec()),
        )
    }
}
