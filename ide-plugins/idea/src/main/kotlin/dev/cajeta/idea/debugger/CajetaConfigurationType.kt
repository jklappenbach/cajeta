package dev.cajeta.idea.debugger

import com.intellij.execution.configurations.ConfigurationFactory
import com.intellij.execution.configurations.ConfigurationType
import com.intellij.execution.configurations.ConfigurationTypeBase
import com.intellij.execution.configurations.RunConfiguration
import com.intellij.openapi.components.BaseState
import com.intellij.openapi.project.Project
import com.intellij.openapi.util.NotNullLazyValue
import dev.cajeta.idea.CajetaIcons

/**
 * The "Cajeta" run-configuration type, offered under Run/Debug
 * Configurations. Carries a single factory that produces
 * [CajetaRunConfiguration]s driven by the [CajetaProgramRunner] under the
 * Debug executor.
 */
class CajetaConfigurationType : ConfigurationTypeBase(
    ID,
    "Cajeta",
    "Debug a Cajeta program via the cajeta dap server",
    NotNullLazyValue.createValue { CajetaIcons.FILE },
) {
    init {
        addFactory(CajetaConfigurationFactory(this))
    }

    companion object {
        const val ID = "CajetaDebugConfiguration"
    }
}

class CajetaConfigurationFactory(type: ConfigurationType) : ConfigurationFactory(type) {

    override fun getId(): String = "CajetaDebugConfigurationFactory"

    override fun createTemplateConfiguration(project: Project): RunConfiguration =
        CajetaRunConfiguration(project, this, "Cajeta")

    override fun getOptionsClass(): Class<out BaseState> = CajetaRunConfigurationOptions::class.java
}
