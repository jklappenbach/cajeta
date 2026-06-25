package dev.cajeta.idea.buildtool

import com.intellij.execution.configurations.RunConfigurationOptions

/**
 * Persisted state for a Cajeta task run configuration (spec §6, §11): the task,
 * the manifest it lives in, and the per-run profile/flavor/`-P`/`-p` selections.
 * Properties and params persist as newline-delimited `key=value` text (parsed by
 * [KvText]).
 */
class CajetaTaskRunConfigurationOptions : RunConfigurationOptions() {

    private val taskOpt = string("").provideDelegate(this, "task")
    private val manifestPathOpt = string("").provideDelegate(this, "manifestPath")
    private val profileOpt = string("").provideDelegate(this, "profile")
    private val flavorOpt = string("").provideDelegate(this, "flavor")
    private val propertiesOpt = string("").provideDelegate(this, "propertiesText")
    private val paramsOpt = string("").provideDelegate(this, "paramsText")
    // §5.2.2 (unit 7): debug-launch coordinates resolved from the project's
    // `settings.build` (entry method JIT-run by `cajeta dap`, source root to
    // compile). Blank entryMethod => not debuggable.
    private val entryMethodOpt = string("").provideDelegate(this, "entryMethod")
    private val sourceRootOpt = string("").provideDelegate(this, "sourceRoot")

    var task: String
        get() = taskOpt.getValue(this) ?: ""
        set(value) = taskOpt.setValue(this, value)

    var manifestPath: String
        get() = manifestPathOpt.getValue(this) ?: ""
        set(value) = manifestPathOpt.setValue(this, value)

    var profile: String
        get() = profileOpt.getValue(this) ?: ""
        set(value) = profileOpt.setValue(this, value)

    var flavor: String
        get() = flavorOpt.getValue(this) ?: ""
        set(value) = flavorOpt.setValue(this, value)

    var propertiesText: String
        get() = propertiesOpt.getValue(this) ?: ""
        set(value) = propertiesOpt.setValue(this, value)

    var paramsText: String
        get() = paramsOpt.getValue(this) ?: ""
        set(value) = paramsOpt.setValue(this, value)

    var entryMethod: String
        get() = entryMethodOpt.getValue(this) ?: ""
        set(value) = entryMethodOpt.setValue(this, value)

    var sourceRoot: String
        get() = sourceRootOpt.getValue(this) ?: ""
        set(value) = sourceRootOpt.setValue(this, value)
}
