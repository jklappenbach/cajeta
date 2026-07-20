package dev.cajeta.idea.debugger

/**
 * The launch coordinates a [CajetaDebugProcess] needs: which entry method to
 * JIT-run, from which source root, and whether to stop on entry. Abstracted to
 * an interface so both the standalone debug run configuration
 * ([CajetaRunConfiguration]) and a build-tool task configuration can drive the
 * same `cajeta dap` path — the server JIT-runs source, so a debuggable task is
 * launched from its project's entry method, not a prebuilt artifact (widget
 * spec §5.2.2).
 */
interface CajetaDebugLaunchSpec {
    val entryMethod: String
    val sourceRoot: String
    val stopOnEntry: Boolean

    /** Environment overlay for the debuggee (spec §4). Defaulted so an
     *  implementation with no environment of its own stays source-compatible. */
    val envVars: Map<String, String> get() = emptyMap()
    val inheritSystemEnv: Boolean get() = true
}
