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
        val session = XDebuggerManager.getInstance(environment.project).startSession(
            environment,
            object : XDebugProcessStarter() {
                override fun start(session: XDebugSession): XDebugProcess =
                    CajetaDebugProcess(session, configuration)
            },
        )
        return resolvedPromise(session.runContentDescriptor)
    }
}
