package dev.cajeta.idea.buildtool

import com.intellij.execution.ProgramRunnerUtil
import com.intellij.execution.RunManager
import com.intellij.execution.configurations.ConfigurationTypeUtil
import com.intellij.execution.executors.DefaultRunExecutor
import com.intellij.openapi.project.Project
import dev.cajeta.idea.settings.CajetaSettings

/**
 * Launches a task from the tool window through the platform Run framework
 * (spec §5, §6): builds an ephemeral [CajetaTaskRunConfiguration] for the node
 * (seeded with the active profile/flavor) and executes it under the Run
 * executor, so it streams to a real ConsoleView with Stop / Re-run / independent
 * multi-tab runs. Unit 7 adds the Debug executor on the same configuration.
 */
object CajetaTaskLauncher {

    fun launch(project: Project, manifestPath: String, node: TaskTreeNode) {
        val type = ConfigurationTypeUtil.findConfigurationType(CajetaTaskConfigurationType::class.java)
            ?: return
        val runManager = RunManager.getInstance(project)
        val settings = runManager.createConfiguration("cajeta ${node.runName}", type.configurationFactories[0])
        val config = settings.configuration as CajetaTaskRunConfiguration
        config.task = node.runName
        config.manifestPath = manifestPath
        config.profile = CajetaSettings.instance.defaultProfile
        config.flavor = CajetaSettings.instance.defaultFlavor

        ProgramRunnerUtil.executeConfiguration(settings, DefaultRunExecutor.getRunExecutorInstance())
    }
}
