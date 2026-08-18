package dev.cajeta.idea.coverage

import com.intellij.notification.NotificationGroupManager
import com.intellij.notification.NotificationType
import com.intellij.openapi.actionSystem.AnActionEvent
import com.intellij.openapi.project.Project
import com.intellij.openapi.project.DumbAwareAction

/**
 * Telling the developer their coverage has gone stale, and offering the one
 * thing that fixes it (spec §5.3).
 *
 * Kept separate from detection because the two have different rhythms: freshness
 * is queried per repaint, while the developer should be told once — repeating it
 * on every keystroke would train them to dismiss it.
 */
object CocoStaleNotifier {

    /**
     * Report the stale files under the current run, with a Re-run action.
     *
     * Returns the message shown, or null when nothing is stale — so callers can
     * assert on the decision without a UI.
     */
    fun notifyIfStale(project: Project, rerun: (() -> Unit)? = null): String? {
        val stale = CocoFreshness.getInstance(project).staleFiles()
        if (stale.isEmpty()) return null

        val text = message(stale)
        val notification = NotificationGroupManager.getInstance()
            .getNotificationGroup(CajetaCoverageProgramRunner.NOTIFICATION_GROUP)
            .createNotification(text, NotificationType.WARNING)
        if (rerun != null) {
            notification.addAction(object : DumbAwareAction("Re-run Coverage") {
                override fun actionPerformed(e: AnActionEvent) {
                    notification.expire()
                    rerun()
                }
            })
        }
        notification.notify(project)
        return text
    }

    /** The wording, separated so it can be asserted on without the platform. */
    fun message(stale: List<String>): String {
        val names = stale.map { it.substringAfterLast('/').substringAfterLast('\\') }
        val shown = names.take(MAX_NAMED).joinToString(", ")
        val rest = names.size - MAX_NAMED
        val which = if (rest > 0) "$shown and $rest more" else shown
        val verb = if (names.size == 1) "has" else "have"
        return "Coverage is stale: $which $verb changed since the run. " +
            "Their markings are hidden rather than drawn against shifted lines."
    }

    private const val MAX_NAMED = 3
}
