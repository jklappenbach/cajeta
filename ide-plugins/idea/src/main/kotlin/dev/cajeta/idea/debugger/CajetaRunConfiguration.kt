package dev.cajeta.idea.debugger

import com.intellij.execution.Executor
import com.intellij.execution.configurations.ConfigurationFactory
import com.intellij.execution.configurations.RunConfiguration
import com.intellij.execution.configurations.RunConfigurationBase
import com.intellij.execution.configurations.RunProfileState
import com.intellij.execution.runners.ExecutionEnvironment
import com.intellij.openapi.options.SettingsEditor
import com.intellij.openapi.project.Project

/**
 * A Cajeta debug run configuration. Holds the launch parameters
 * (entry method, source root, stop-on-entry) as typed accessors over the
 * persisted [CajetaRunConfigurationOptions].
 *
 * Execution is handled by [CajetaProgramRunner], which starts an
 * XDebugSession directly; [getState] therefore returns a no-op profile state
 * (we don't run a console process — the `cajeta dap` server is launched and
 * driven by the debug process).
 */
class CajetaRunConfiguration(
    project: Project,
    factory: ConfigurationFactory,
    name: String,
) : RunConfigurationBase<CajetaRunConfigurationOptions>(project, factory, name) {

    public override fun getOptions(): CajetaRunConfigurationOptions =
        super.getOptions() as CajetaRunConfigurationOptions

    var entryMethod: String
        get() = options.entryMethod
        set(value) { options.entryMethod = value }

    var sourceRoot: String
        get() = options.sourceRoot
        set(value) { options.sourceRoot = value }

    var stopOnEntry: Boolean
        get() = options.stopOnEntry
        set(value) { options.stopOnEntry = value }

    override fun getConfigurationEditor(): SettingsEditor<out RunConfiguration> =
        CajetaRunConfigurationEditor()

    override fun getState(executor: Executor, environment: ExecutionEnvironment): RunProfileState =
        RunProfileState { _, _ -> null }
}
