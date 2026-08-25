package dev.cajeta.idea.profiler

import com.intellij.execution.ExecutionListener
import com.intellij.execution.configurations.RunProfile
import com.intellij.execution.process.ProcessHandler
import com.intellij.execution.runners.ExecutionEnvironment
import com.intellij.openapi.application.ApplicationManager
import java.io.File

/**
 * cajeta-profiler 11.2.e — a profiled run opens its own trace (spec §8.1, §9.5).
 *
 * The trace is written at process exit. Without this, a developer who ticked
 * "profile this run" watches it finish and then has to go and find a file whose
 * path they never chose — which is most of the friction §9.5 exists to remove.
 *
 * Only runs this plugin ARMED are picked up. Deciding from the file alone —
 * "a .pftrace appeared, show it" — would hijack unrelated runs and would race
 * anything else writing traces in the same project.
 */
class ProfiledRunListener : ExecutionListener {

    override fun processStarted(
        executorId: String,
        env: ExecutionEnvironment,
        handler: ProcessHandler,
    ) {
        val trace = armedTraceOf(env.runProfile) ?: return
        // Recorded at START. By the time the process ends the configuration may
        // have been edited, and the file the run actually wrote is the one named
        // in the environment it was started with.
        handler.putUserData(TRACE_KEY, trace)
    }

    override fun processTerminated(
        executorId: String,
        env: ExecutionEnvironment,
        handler: ProcessHandler,
        exitCode: Int,
    ) {
        val project = env.project
        val trace = handler.getUserData(TRACE_KEY) ?: return
        // A crashed run still writes whatever it had: the trace transform runs
        // from an atexit path, and a truncated trace is read for the packets it
        // does contain (§7.7). So the exit code is not consulted — a profile of
        // a run that died is frequently the profile someone most wants.
        ApplicationManager.getApplication().invokeLater {
            if (project.isDisposed) return@invokeLater
            if (trace.isFile) CajetaProfilerToolWindow.open(project, trace)
        }
    }

    private fun armedTraceOf(profile: RunProfile?): File? {
        val env = (profile as? EnvironmentAware)?.envVars ?: return null
        if (!CajetaProfileLocation.isArmed(env)) return null
        val out = env[CajetaProfileLocation.OUT] ?: return null
        return File(out)
    }

    /**
     * The shape this listener needs from a configuration.
     *
     * Declared here rather than depending on a concrete configuration class so
     * a new one — a build-tool task, a test run — becomes profilable by
     * carrying its environment, without this file learning about it.
     */
    interface EnvironmentAware {
        val envVars: Map<String, String>
    }

    private companion object {
        val TRACE_KEY = com.intellij.openapi.util.Key.create<File>("cajeta.profiler.trace")
    }
}
