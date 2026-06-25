package dev.cajeta.idea.buildtool

import com.intellij.notification.NotificationGroupManager
import com.intellij.notification.NotificationType
import com.intellij.openapi.progress.ProgressIndicator
import com.intellij.openapi.progress.ProgressManager
import com.intellij.openapi.progress.Task
import com.intellij.openapi.project.Project
import dev.cajeta.idea.settings.CajetaSettings

/**
 * Launches a task from the tool window. Unit 5 ships a minimal background
 * launcher (off the EDT, reports the exit code via a notification); unit 6
 * replaces this with a real RunConfiguration + streaming ConsoleView and unit 12
 * adds the profile/flavor/args overrides. Centralized here so the upgrade is a
 * single seam.
 */
object CajetaTaskLauncher {

    private fun group() =
        NotificationGroupManager.getInstance().getNotificationGroup("Cajeta Build Tool")

    fun specFor(manifestPath: String, node: TaskTreeNode): TaskRunSpec {
        val s = CajetaSettings.instance
        return TaskRunSpec(
            task = node.runName,
            manifestPath = manifestPath,
            profile = s.defaultProfile.ifBlank { null },
            flavor = s.defaultFlavor.ifBlank { null },
        )
    }

    fun launch(project: Project, manifestPath: String, node: TaskTreeNode) {
        val s = CajetaSettings.instance
        val argv = listOf(s.buildToolPath) + CajetaCommandLine.runArgv(specFor(manifestPath, node))
        ProgressManager.getInstance().run(
            object : Task.Backgroundable(project, "cajeta ${node.runName}", true) {
                override fun run(indicator: ProgressIndicator) {
                    indicator.text = argv.joinToString(" ")
                    val result = CajetaBuildRunner.spawn(argv, timeoutMs = 10 * 60_000)
                    val (type, msg) = when {
                        result.timedOut -> NotificationType.ERROR to "timed out"
                        result.exitCode == 0 -> NotificationType.INFORMATION to "succeeded"
                        else -> NotificationType.ERROR to "exited ${result.exitCode}"
                    }
                    group().createNotification("cajeta ${node.runName} $msg",
                        result.stderr.trim().take(400), type).notify(project)
                }
            },
        )
    }
}
