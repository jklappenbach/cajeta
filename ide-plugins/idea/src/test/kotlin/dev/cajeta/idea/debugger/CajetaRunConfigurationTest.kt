package dev.cajeta.idea.debugger

import com.intellij.execution.configurations.ConfigurationTypeUtil
import com.intellij.execution.executors.DefaultDebugExecutor
import com.intellij.execution.executors.DefaultRunExecutor
import com.intellij.testFramework.fixtures.BasePlatformTestCase

/**
 * Platform-fixture test (CP6b): the run-configuration type is registered, its
 * options round-trip through the typed accessors, and the program runner gates
 * on the Debug executor + a Cajeta configuration. Runs headless under the
 * IntelliJ test fixture; full XDebugSession execution is a runIde smoke.
 */
class CajetaRunConfigurationTest : BasePlatformTestCase() {

    fun testConfigurationTypeIsRegistered() {
        val type = ConfigurationTypeUtil.findConfigurationType(CajetaConfigurationType::class.java)
        assertNotNull("CajetaConfigurationType not registered", type)
        assertEquals(1, type!!.configurationFactories.size)
        assertEquals("Cajeta", type.displayName)
    }

    fun testOptionsRoundTrip() {
        val type = ConfigurationTypeUtil.findConfigurationType(CajetaConfigurationType::class.java)!!
        val config = type.configurationFactories[0]
            .createTemplateConfiguration(project) as CajetaRunConfiguration

        config.entryMethod = "demo.Calc.main"
        config.sourceRoot = "/tmp/src"
        config.stopOnEntry = true

        assertEquals("demo.Calc.main", config.entryMethod)
        assertEquals("/tmp/src", config.sourceRoot)
        assertTrue(config.stopOnEntry)
    }

    /** 5.1.1 — environment entries and the inherit flag persist like the rest. */
    fun testEnvironmentRoundTrips() {
        val config = templateConfig()

        config.envVars = linkedMapOf("FOO" to "1", "BAR" to "two")
        config.inheritSystemEnv = false

        assertEquals(mapOf("FOO" to "1", "BAR" to "two"), config.envVars)
        assertFalse(config.inheritSystemEnv)
    }

    /**
     * 5.1.1 — the defaults matter as much as the round-trip: an existing
     * configuration deserialized from disk has no env state at all, and must
     * come back as "no entries, inherit the shell" — the behaviour it had
     * before this feature existed (spec 4.1.6).
     */
    fun testEnvironmentDefaultsToEmptyAndInheriting() {
        val config = templateConfig()

        assertTrue(config.envVars.isEmpty())
        assertTrue(config.inheritSystemEnv)
    }

    private fun templateConfig(): CajetaRunConfiguration {
        val type = ConfigurationTypeUtil.findConfigurationType(CajetaConfigurationType::class.java)!!
        return type.configurationFactories[0]
            .createTemplateConfiguration(project) as CajetaRunConfiguration
    }

    fun testRunnerGatesOnDebugExecutorAndCajetaConfig() {
        val runner = CajetaProgramRunner()
        val type = ConfigurationTypeUtil.findConfigurationType(CajetaConfigurationType::class.java)!!
        val config = type.configurationFactories[0]
            .createTemplateConfiguration(project) as CajetaRunConfiguration

        assertTrue(runner.canRun(DefaultDebugExecutor.EXECUTOR_ID, config))
        assertFalse("should not handle plain Run", runner.canRun(DefaultRunExecutor.EXECUTOR_ID, config))
    }
}
