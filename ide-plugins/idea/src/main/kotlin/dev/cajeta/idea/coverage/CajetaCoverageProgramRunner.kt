package dev.cajeta.idea.coverage

import com.intellij.coverage.CoverageExecutor
import com.intellij.execution.configurations.RunProfile
import com.intellij.execution.configurations.RunProfileState
import com.intellij.execution.configurations.RunnerSettings
import com.intellij.execution.process.ProcessEvent
import com.intellij.execution.process.ProcessListener
import com.intellij.execution.runners.AsyncProgramRunner
import com.intellij.execution.runners.ExecutionEnvironment
import com.intellij.execution.runners.RunContentBuilder
import com.intellij.execution.ui.RunContentDescriptor
import com.intellij.notification.NotificationGroupManager
import com.intellij.notification.NotificationType
import com.intellij.openapi.fileEditor.FileDocumentManager
import dev.cajeta.idea.buildtool.CajetaTaskRunConfiguration
import org.jetbrains.concurrency.Promise
import org.jetbrains.concurrency.resolvedPromise
import java.io.File

/**
 * Run with Coverage for a Cajeta build-tool task (spec §4.1).
 *
 * Runs the task exactly as the ordinary Run executor would — same argv, same
 * console — and, when the process exits, discovers the artifacts coco wrote and
 * loads them. Nothing about the command line changes: measuring is the task's
 * job, declared in the manifest as a `cajeta.coverage.instrument` action, not
 * something the IDE injects. That keeps the IDE run and the CI run the same run.
 *
 * The three ways this can fail all report rather than silently yielding no
 * coverage (spec §4.2, §4.3):
 *
 *  - the project does not use coco → say so, and name what to declare;
 *  - the chosen task does not instrument → say which tasks do;
 *  - the run finished but produced nothing readable → say where it looked.
 */
class CajetaCoverageProgramRunner : AsyncProgramRunner<RunnerSettings>() {

    override fun getRunnerId(): String = "CajetaCoverageRunner"

    override fun canRun(executorId: String, profile: RunProfile): Boolean =
        executorId == CoverageExecutor.EXECUTOR_ID && profile is CajetaTaskRunConfiguration

    override fun execute(
        environment: ExecutionEnvironment,
        state: RunProfileState,
    ): Promise<RunContentDescriptor?> {
        FileDocumentManager.getInstance().saveAllDocuments()
        val configuration = environment.runProfile as CajetaTaskRunConfiguration
        val project = environment.project

        val setup = CocoProject.of(project)
        preflight(setup, configuration.task)?.let { problem ->
            notify(environment, problem, NotificationType.WARNING)
            return resolvedPromise(null)
        }

        val result = state.execute(environment.executor, this)
            ?: return resolvedPromise(null)
        val outDir = resolveOutDir(configuration, setup)

        result.processHandler?.addProcessListener(object : ProcessListener {
            override fun processTerminated(event: ProcessEvent) {
                // A failed run has no coverage to load, and saying "no coverage
                // found" over the top of a build failure buries the real cause.
                if (event.exitCode != 0) {
                    notify(
                        environment,
                        "The coverage run failed (exit ${event.exitCode}). " +
                            "See the run console for coco's output.",
                        NotificationType.ERROR,
                    )
                    return
                }
                CocoRunLoader.loadInBackground(project, outDir, configuration.name) { outcome ->
                    notify(
                        environment,
                        CocoRunLoader.describe(outcome),
                        if (outcome is CocoRunLoader.Outcome.Loaded) NotificationType.INFORMATION
                        else NotificationType.WARNING,
                    )
                }
            }
        })

        return resolvedPromise(
            RunContentBuilder(result, environment).showRunContent(environment.contentToReuse)
        )
    }

    /** Null when the run may proceed; otherwise the reason it may not. */
    private fun preflight(setup: CocoProject, task: String): String? {
        if (!setup.isConfigured) return setup.problem
        if (task.isNotBlank() && task !in setup.coverageTasks) {
            return "The task \"$task\" does not measure coverage. " +
                "Tasks that do: ${setup.coverageTasks.joinToString(", ")}."
        }
        return null
    }

    /**
     * coco's `out` is relative to the manifest directory, which is also the
     * task's working directory.
     */
    private fun resolveOutDir(configuration: CajetaTaskRunConfiguration, setup: CocoProject): File {
        val base = configuration.manifestPath.ifBlank { null }?.let { File(it).parentFile }
            ?: configuration.project.basePath?.let(::File)
            ?: File(".")
        val out = File(setup.outDir)
        return if (out.isAbsolute) out else File(base, setup.outDir)
    }

    private fun notify(environment: ExecutionEnvironment, text: String, type: NotificationType) {
        NotificationGroupManager.getInstance()
            .getNotificationGroup(NOTIFICATION_GROUP)
            .createNotification(text, type)
            .notify(environment.project)
    }

    companion object {
        const val NOTIFICATION_GROUP: String = "Cajeta Coverage"
    }
}
