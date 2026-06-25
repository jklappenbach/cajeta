package dev.cajeta.idea.buildtool

import com.intellij.execution.configurations.ConfigurationFactory
import com.intellij.execution.configurations.ConfigurationType
import com.intellij.execution.configurations.ConfigurationTypeBase
import com.intellij.execution.configurations.RunConfiguration
import com.intellij.openapi.components.BaseState
import com.intellij.openapi.project.Project
import com.intellij.openapi.util.NotNullLazyValue
import dev.cajeta.idea.CajetaIcons

/**
 * The "Cajeta Task" run-configuration type — runs a `cajeta` build-tool task
 * with a streaming console (spec §6). Distinct from the debug-program type
 * (`CajetaConfigurationType`); a task run can also be debugged via unit 7.
 */
class CajetaTaskConfigurationType : ConfigurationTypeBase(
    ID,
    "Cajeta Task",
    "Run a Cajeta build-tool task",
    NotNullLazyValue.createValue { CajetaIcons.FILE },
) {
    init {
        addFactory(CajetaTaskConfigurationFactory(this))
    }

    companion object {
        const val ID = "CajetaTaskConfiguration"
    }
}

class CajetaTaskConfigurationFactory(type: ConfigurationType) : ConfigurationFactory(type) {

    override fun getId(): String = "CajetaTaskConfigurationFactory"

    override fun createTemplateConfiguration(project: Project): RunConfiguration =
        CajetaTaskRunConfiguration(project, this, "Cajeta Task")

    override fun getOptionsClass(): Class<out BaseState> =
        CajetaTaskRunConfigurationOptions::class.java
}
