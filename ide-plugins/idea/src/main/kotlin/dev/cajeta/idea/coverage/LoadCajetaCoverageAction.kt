package dev.cajeta.idea.coverage

import com.intellij.notification.NotificationGroupManager
import com.intellij.notification.NotificationType
import com.intellij.openapi.actionSystem.ActionUpdateThread
import com.intellij.openapi.actionSystem.AnAction
import com.intellij.openapi.actionSystem.AnActionEvent
import com.intellij.openapi.actionSystem.CommonDataKeys
import com.intellij.openapi.project.Project
import java.io.File

/**
 * Load a previous coco run's results without running anything (spec §4.5).
 *
 * The platform's own Import Coverage action exists, but it opens a file chooser
 * and expects the user to know that the file wanted is
 * `build/coco/run/coco.profile` rather than the `sites.tsv` sitting next to it.
 * The location is derivable from the manifest, so deriving it is better than
 * asking.
 *
 * This is also what makes re-examination free: nothing here runs the pipeline,
 * so looking at yesterday's numbers costs a file read (spec §4.4, §7.2).
 */
class LoadCajetaCoverageAction : AnAction() {

    override fun getActionUpdateThread(): ActionUpdateThread = ActionUpdateThread.BGT

    override fun update(e: AnActionEvent) {
        val project = e.getData(CommonDataKeys.PROJECT)
        e.presentation.isEnabledAndVisible = project != null && CocoProject.of(project).isConfigured
    }

    override fun actionPerformed(e: AnActionEvent) {
        val project = e.getData(CommonDataKeys.PROJECT) ?: return
        val setup = CocoProject.of(project)
        if (!setup.isConfigured) {
            notify(project, setup.problem ?: "coverage is not configured", NotificationType.WARNING)
            return
        }
        val outDir = outDirOf(project, setup)
        CocoRunLoader.loadInBackground(project, outDir, "coco") { outcome ->
            notify(
                project,
                CocoRunLoader.describe(outcome),
                if (outcome is CocoRunLoader.Outcome.Loaded) NotificationType.INFORMATION
                else NotificationType.WARNING,
            )
        }
    }

    private fun outDirOf(project: Project, setup: CocoProject): File {
        val base = dev.cajeta.idea.buildtool.CajetaManifest.path(project)
            ?.let { File(it).parentFile }
            ?: project.basePath?.let(::File)
            ?: File(".")
        val out = File(setup.outDir)
        return if (out.isAbsolute) out else File(base, setup.outDir)
    }

    private fun notify(project: Project, text: String, type: NotificationType) {
        NotificationGroupManager.getInstance()
            .getNotificationGroup(CajetaCoverageProgramRunner.NOTIFICATION_GROUP)
            .createNotification(text, type)
            .notify(project)
    }
}
