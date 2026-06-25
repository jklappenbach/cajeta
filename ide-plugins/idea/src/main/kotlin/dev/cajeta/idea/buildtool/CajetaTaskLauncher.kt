package dev.cajeta.idea.buildtool

import com.intellij.execution.ProgramRunnerUtil
import com.intellij.execution.RunManager
import com.intellij.execution.RunnerAndConfigurationSettings
import com.intellij.execution.configurations.ConfigurationTypeUtil
import com.intellij.execution.executors.DefaultDebugExecutor
import com.intellij.execution.executors.DefaultRunExecutor
import com.intellij.openapi.project.Project
import dev.cajeta.idea.settings.CajetaSettings

/**
 * Launches a task from the tool window through the platform Run framework
 * (spec §5, §6): builds an ephemeral [CajetaTaskRunConfiguration] for the node
 * (seeded with the active profile/flavor) and executes it under the Run
 * executor, so it streams to a real ConsoleView with Stop / Re-run / independent
 * multi-tab runs. Unit 7 adds the Debug executor on the same configuration: the
 * task's project entry method / source root (from discovery) are stamped on the
 * config and [CajetaProgramRunner] drives the `cajeta dap` session.
 */
object CajetaTaskLauncher {

    fun launch(project: Project, manifestPath: String, node: TaskTreeNode) {
        val settings = buildConfig(project, manifestPath, node) ?: return
        ProgramRunnerUtil.executeConfiguration(settings, DefaultRunExecutor.getRunExecutorInstance())
    }

    /** Run a task with an explicit [TaskRunSpec] from the Run-with-args dialog
     *  (spec §12): the profile/flavor/properties/params override the defaults. */
    fun launchWithSpec(project: Project, spec: TaskRunSpec) {
        val type = ConfigurationTypeUtil.findConfigurationType(CajetaTaskConfigurationType::class.java)
            ?: return
        val runManager = RunManager.getInstance(project)
        val settings = runManager.createConfiguration("cajeta ${spec.task}", type.configurationFactories[0])
        val config = settings.configuration as CajetaTaskRunConfiguration
        config.task = spec.task
        config.manifestPath = spec.manifestPath ?: ""
        config.profile = spec.profile ?: ""
        config.flavor = spec.flavor ?: ""
        config.propertiesText = KvText.format(spec.properties)
        config.paramsText = KvText.format(spec.params)
        ProgramRunnerUtil.executeConfiguration(settings, DefaultRunExecutor.getRunExecutorInstance())
    }

    /**
     * Debug the task under the existing `cajeta dap` path (spec §5.2.2). The
     * [model] supplies the project's debug-launch coordinates; a task that
     * isn't debuggable (no entry method, or not runnable) is a no-op here — the
     * caller disables Debug in the menu, but we guard again so a stale call
     * can't launch a session that cannot attach.
     */
    fun debug(project: Project, manifestPath: String, model: TaskModel, node: TaskTreeNode) {
        val task = model.tasks.firstOrNull { it.name == node.runName } ?: return
        val launch = TaskDebugMapping.launchFor(task, model, manifestPath) ?: return
        val settings = buildConfig(project, manifestPath, node) ?: return
        val config = settings.configuration as CajetaTaskRunConfiguration
        config.entryMethod = launch.entryMethod
        config.sourceRoot = launch.sourceRoot
        ProgramRunnerUtil.executeConfiguration(settings, DefaultDebugExecutor.getDebugExecutorInstance())
    }

    /** Persist a task (with its active bindings) as a saved run configuration
     *  (spec §11.2.1) — editable in Run/Debug Configurations, not ephemeral. */
    fun saveConfig(project: Project, spec: TaskRunSpec): RunnerAndConfigurationSettings? {
        val type = ConfigurationTypeUtil.findConfigurationType(CajetaTaskConfigurationType::class.java)
            ?: return null
        val runManager = RunManager.getInstance(project)
        val settings = runManager.createConfiguration("cajeta ${spec.task}", type.configurationFactories[0])
        val config = settings.configuration as CajetaTaskRunConfiguration
        config.task = spec.task
        config.manifestPath = spec.manifestPath ?: ""
        config.profile = spec.profile ?: ""
        config.flavor = spec.flavor ?: ""
        config.propertiesText = KvText.format(spec.properties)
        config.paramsText = KvText.format(spec.params)
        runManager.addConfiguration(settings)
        runManager.selectedConfiguration = settings
        return settings
    }

    private fun buildConfig(
        project: Project,
        manifestPath: String,
        node: TaskTreeNode,
    ): RunnerAndConfigurationSettings? {
        val type = ConfigurationTypeUtil.findConfigurationType(CajetaTaskConfigurationType::class.java)
            ?: return null
        val runManager = RunManager.getInstance(project)
        val settings = runManager.createConfiguration("cajeta ${node.runName}", type.configurationFactories[0])
        val config = settings.configuration as CajetaTaskRunConfiguration
        config.task = node.runName
        config.manifestPath = manifestPath
        config.profile = CajetaSettings.instance.defaultProfile
        config.flavor = CajetaSettings.instance.defaultFlavor
        return settings
    }
}
