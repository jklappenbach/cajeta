package dev.cajeta.idea.debugger

import com.intellij.execution.configurations.RunConfigurationOptions

/**
 * Persisted state for a Cajeta debug run configuration: which entry method to
 * run, the source root to compile, and whether to stop on entry. Forwarded to
 * the `cajeta dap` launch request.
 */
class CajetaRunConfigurationOptions : RunConfigurationOptions() {

    private val entryMethodOpt = string("").provideDelegate(this, "entryMethod")
    private val sourceRootOpt = string("").provideDelegate(this, "sourceRoot")
    private val stopOnEntryOpt = property(false).provideDelegate(this, "stopOnEntry")

    var entryMethod: String
        get() = entryMethodOpt.getValue(this) ?: ""
        set(value) = entryMethodOpt.setValue(this, value)

    var sourceRoot: String
        get() = sourceRootOpt.getValue(this) ?: ""
        set(value) = sourceRootOpt.setValue(this, value)

    var stopOnEntry: Boolean
        get() = stopOnEntryOpt.getValue(this)
        set(value) = stopOnEntryOpt.setValue(this, value)
}
