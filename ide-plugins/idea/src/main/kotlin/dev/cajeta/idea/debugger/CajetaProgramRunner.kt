package dev.cajeta.idea.debugger

import com.intellij.execution.configurations.RunProfile
import com.intellij.execution.configurations.RunProfileState
import com.intellij.execution.configurations.RunnerSettings
import com.intellij.execution.executors.DefaultDebugExecutor
import com.intellij.execution.runners.AsyncProgramRunner
import com.intellij.execution.runners.ExecutionEnvironment
import com.intellij.execution.ui.RunContentDescriptor
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.xdebugger.XDebugProcess
import com.intellij.xdebugger.XDebugProcessStarter
import com.intellij.xdebugger.XDebugSession
import com.intellij.xdebugger.XDebuggerManager
import org.jetbrains.concurrency.Promise
import org.jetbrains.concurrency.resolvedPromise

/**
 * Program runner for the Debug executor: starts an XDebugSession backed by a
 * [CajetaDebugProcess]. Handles the Debug executor for any
 * [CajetaDebugLaunchSpec] that carries an entry method — the standalone
 * [CajetaRunConfiguration] and a debuggable build-tool task configuration alike
 * (widget §5.2.2). A task without launch coordinates reports a blank entry
 * method and is not debuggable, so the platform keeps Debug disabled. The
 * platform's default runner handles plain Run.
 */
class CajetaProgramRunner : AsyncProgramRunner<RunnerSettings>() {

    override fun getRunnerId(): String = "CajetaDebugRunner"

    override fun canRun(executorId: String, profile: RunProfile): Boolean {
        if (executorId != DefaultDebugExecutor.EXECUTOR_ID) return false
        if (profile !is CajetaDebugLaunchSpec) return false
        // The standalone debug config is debuggable by intent even when its
        // entry method is still blank (filled in the editor). Any other launch
        // spec — a build-tool task config — is debuggable only once discovery
        // has stamped the project's entry method on it (§5.2.2).
        return profile is CajetaRunConfiguration || profile.entryMethod.isNotBlank()
    }

    override fun execute(
        environment: ExecutionEnvironment,
        state: RunProfileState,
    ): Promise<RunContentDescriptor?> {
        FileDocumentManager.getInstance().saveAllDocuments()
        val configuration = environment.runProfile as CajetaDebugLaunchSpec
        val starter = object : XDebugProcessStarter() {
            override fun start(session: XDebugSession): XDebugProcess =
                CajetaDebugProcess(session, configuration)
        }
        return resolvedPromise(startSessionForDescriptor(environment, starter))
    }

    /**
     * Start the XDebugSession and return the [RunContentDescriptor] the
     * platform expects back from [execute].
     *
     * Since 2026.1 the debugger runs split (frontend/backend), and reading the
     * descriptor off the session logs a platform error dialog: the session's
     * descriptor there is a backend-side mock, not the UI descriptor. The
     * documented replacement (XDebugSession.getRunContentDescriptor's javadoc)
     * is `XDebuggerManager.newSessionBuilder(...).startSession()`, whose
     * XSessionStartedResult carries the right descriptor for the mode. That
     * API does not exist in the 2025.2 SDK this plugin compiles against, so it
     * is reached by reflection; a platform without it takes the old path,
     * where the session's descriptor is the real one and reading it is silent.
     *
     * The API-present probe is separated from the invocation on purpose: a
     * launch that fails through the builder must SURFACE, not fall back and
     * start a second session through the legacy path.
     */
    private fun startSessionForDescriptor(
        environment: ExecutionEnvironment,
        starter: XDebugProcessStarter,
    ): RunContentDescriptor? {
        val manager = XDebuggerManager.getInstance(environment.project)
        val newSessionBuilder = try {
            XDebuggerManager::class.java
                .getMethod("newSessionBuilder", XDebugProcessStarter::class.java)
        } catch (_: NoSuchMethodException) {
            null
        }
        if (newSessionBuilder == null) {
            // Pre-split platform (2025.x and earlier).
            return manager.startSession(environment, starter).runContentDescriptor
        }
        try {
            val builderClass = newSessionBuilder.returnType // XDebugSessionBuilder
            val builder = newSessionBuilder.invoke(manager, starter)
            val configured = builderClass
                .getMethod("environment", ExecutionEnvironment::class.java)
                .invoke(builder, environment)
            val startSession = builderClass.getMethod("startSession")
            val result = startSession.invoke(configured) // XSessionStartedResult
            return startSession.returnType
                .getMethod("getRunContentDescriptor")
                .invoke(result) as RunContentDescriptor?
        } catch (e: java.lang.reflect.InvocationTargetException) {
            // Unwrap so an ExecutionException from the launch reads as itself,
            // not as a reflection artifact.
            throw e.cause ?: e
        }
    }
}
