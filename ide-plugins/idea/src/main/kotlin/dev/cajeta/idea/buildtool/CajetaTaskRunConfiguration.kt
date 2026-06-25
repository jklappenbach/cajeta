package dev.cajeta.idea.buildtool

import com.intellij.execution.Executor
import com.intellij.execution.configurations.ConfigurationFactory
import com.intellij.execution.configurations.GeneralCommandLine
import com.intellij.execution.configurations.RunConfiguration
import com.intellij.execution.configurations.RunConfigurationBase
import com.intellij.execution.configurations.RunProfileState
import com.intellij.execution.process.KillableColoredProcessHandler
import com.intellij.execution.process.ProcessHandler
import com.intellij.execution.process.ProcessTerminatedListener
import com.intellij.execution.runners.ExecutionEnvironment
import com.intellij.execution.configurations.CommandLineState
import com.intellij.openapi.options.SettingsEditor
import com.intellij.openapi.project.Project
import dev.cajeta.idea.debugger.CajetaDebugLaunchSpec
import dev.cajeta.idea.settings.CajetaSettings
import java.io.File

/**
 * A Cajeta task run configuration (spec §5, §6): runs `cajeta <task> …` and
 * streams output to the platform ConsoleView. The platform's Run framework
 * provides Stop, Re-run, and independent multi-tab runs for free (§6.2). Holds
 * the task + manifest + profile/flavor/properties/params over the persisted
 * [CajetaTaskRunConfigurationOptions]; the command line is derived by the shared
 * pure [CajetaCommandLine] (§4.1.1).
 */
class CajetaTaskRunConfiguration(
    project: Project,
    factory: ConfigurationFactory,
    name: String,
) : RunConfigurationBase<CajetaTaskRunConfigurationOptions>(project, factory, name), CajetaDebugLaunchSpec {

    public override fun getOptions(): CajetaTaskRunConfigurationOptions =
        super.getOptions() as CajetaTaskRunConfigurationOptions

    var task: String
        get() = options.task
        set(value) { options.task = value }

    var manifestPath: String
        get() = options.manifestPath
        set(value) { options.manifestPath = value }

    var profile: String
        get() = options.profile
        set(value) { options.profile = value }

    var flavor: String
        get() = options.flavor
        set(value) { options.flavor = value }

    var propertiesText: String
        get() = options.propertiesText
        set(value) { options.propertiesText = value }

    var paramsText: String
        get() = options.paramsText
        set(value) { options.paramsText = value }

    // CajetaDebugLaunchSpec (§5.2.2): debug-launch coordinates the `cajeta dap`
    // path consumes. Blank entryMethod => not debuggable (CajetaProgramRunner
    // keeps Debug disabled). stopOnEntry is always off for a task launch.
    override var entryMethod: String
        get() = options.entryMethod
        set(value) { options.entryMethod = value }

    override var sourceRoot: String
        get() = options.sourceRoot
        set(value) { options.sourceRoot = value }

    override val stopOnEntry: Boolean get() = false

    /** The shared run spec — the single source of the argv (§4.1.1). */
    fun toRunSpec(): TaskRunSpec = TaskRunSpec(
        task = task,
        manifestPath = manifestPath.ifBlank { null },
        profile = profile.ifBlank { null },
        flavor = flavor.ifBlank { null },
        properties = KvText.parse(propertiesText),
        params = KvText.parse(paramsText),
    )

    override fun getConfigurationEditor(): SettingsEditor<out RunConfiguration> =
        CajetaTaskRunConfigurationEditor()

    override fun getState(executor: Executor, environment: ExecutionEnvironment): RunProfileState =
        object : CommandLineState(environment) {
            override fun startProcess(): ProcessHandler {
                val settings = CajetaSettings.instance
                val cmd = GeneralCommandLine(settings.buildToolPath)
                    .withParameters(CajetaCommandLine.runArgv(toRunSpec()))
                manifestPath.ifBlank { null }?.let { File(it).parent }?.let { cmd.withWorkDirectory(it) }
                val handler = KillableColoredProcessHandler(cmd)
                ProcessTerminatedListener.attach(handler)
                return handler
            }
        }
}
