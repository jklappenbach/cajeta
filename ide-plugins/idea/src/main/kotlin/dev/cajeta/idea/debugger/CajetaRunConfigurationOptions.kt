package dev.cajeta.idea.debugger

import com.intellij.execution.configurations.RunConfigurationOptions

/**
 * Persisted state for a Cajeta debug run configuration: which entry method to
 * run, the source root to compile, whether to stop on entry, and the
 * environment to run under. Forwarded to the `cajeta dap` launch request.
 */
class CajetaRunConfigurationOptions : RunConfigurationOptions() {

    private val entryMethodOpt = string("").provideDelegate(this, "entryMethod")
    private val sourceRootOpt = string("").provideDelegate(this, "sourceRoot")
    private val stopOnEntryOpt = property(false).provideDelegate(this, "stopOnEntry")
    private val envVarsOpt = map<String, String>().provideDelegate(this, "envVars")
    // Defaults true: a configuration written before this field existed
    // deserializes without it, and must keep behaving as it did — inheriting
    // the shell (spec 4.1.6).
    private val inheritSystemEnvOpt = property(true).provideDelegate(this, "inheritSystemEnv")

    var entryMethod: String
        get() = entryMethodOpt.getValue(this) ?: ""
        set(value) = entryMethodOpt.setValue(this, value)

    var sourceRoot: String
        get() = sourceRootOpt.getValue(this) ?: ""
        set(value) = sourceRootOpt.setValue(this, value)

    var stopOnEntry: Boolean
        get() = stopOnEntryOpt.getValue(this)
        set(value) = stopOnEntryOpt.setValue(this, value)

    /** Insertion order is preserved so the editor's table round-trips as typed. */
    var envVars: Map<String, String>
        get() = LinkedHashMap(envVarsOpt.getValue(this))
        set(value) {
            val m = envVarsOpt.getValue(this)
            m.clear()
            m.putAll(value)
        }

    var inheritSystemEnv: Boolean
        get() = inheritSystemEnvOpt.getValue(this)
        set(value) = inheritSystemEnvOpt.setValue(this, value)
}
