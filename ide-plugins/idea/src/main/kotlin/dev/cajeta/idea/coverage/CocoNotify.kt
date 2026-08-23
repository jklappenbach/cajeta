package dev.cajeta.idea.coverage

import com.intellij.notification.NotificationGroupManager
import com.intellij.notification.NotificationType
import com.intellij.openapi.project.Project

/** One place for the coco tool window's own notifications. */
object CocoNotify {
    fun info(project: Project, text: String) {
        NotificationGroupManager.getInstance()
            .getNotificationGroup(CajetaCoverageProgramRunner.NOTIFICATION_GROUP)
            .createNotification(text, NotificationType.INFORMATION)
            .notify(project)
    }
}
