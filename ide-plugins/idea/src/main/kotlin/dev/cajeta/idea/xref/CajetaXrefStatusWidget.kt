package dev.cajeta.idea.xref

import com.intellij.openapi.project.Project
import com.intellij.openapi.util.NlsContexts
import com.intellij.openapi.wm.StatusBar
import com.intellij.openapi.wm.StatusBarWidget
import com.intellij.openapi.wm.StatusBarWidgetFactory
import com.intellij.util.Consumer
import java.awt.event.MouseEvent

/**
 * Visible degradation (ide-symbol-index 9.2.3): a status indicator with an
 * actionable reason, not silence. Text reflects [CajetaXrefFreshness]; the
 * tooltip carries the WHY when navigation is degraded.
 */
class CajetaXrefStatusWidgetFactory : StatusBarWidgetFactory {
    override fun getId(): String = WIDGET_ID
    override fun getDisplayName(): String = "Cajeta Xref"
    override fun isAvailable(project: Project): Boolean = true
    override fun createWidget(project: Project): StatusBarWidget =
        CajetaXrefStatusWidget(project)
    override fun disposeWidget(widget: StatusBarWidget) {}
    override fun canBeEnabledOn(statusBar: StatusBar): Boolean = true

    companion object {
        const val WIDGET_ID = "CajetaXrefStatus"
    }
}

private class CajetaXrefStatusWidget(private val project: Project) :
    StatusBarWidget, StatusBarWidget.TextPresentation {

    override fun ID(): String = CajetaXrefStatusWidgetFactory.WIDGET_ID
    override fun getPresentation(): StatusBarWidget.WidgetPresentation = this
    override fun install(statusBar: StatusBar) {}
    override fun dispose() {}

    override fun getText(): String {
        val f = CajetaXrefFreshness.getInstance(project)
        return when (f.state) {
            CajetaXrefFreshness.State.FRESH -> "cajeta: xref ✓"
            CajetaXrefFreshness.State.REFRESHING -> "cajeta: xref …"
            CajetaXrefFreshness.State.STALE -> "cajeta: xref stale"
            CajetaXrefFreshness.State.UNAVAILABLE -> "cajeta: xref off"
        }
    }

    override fun getTooltipText(): @NlsContexts.Tooltip String? {
        val f = CajetaXrefFreshness.getInstance(project)
        return f.reason ?: "Cajeta cross-reference index: ${f.state.name.lowercase()}"
    }

    override fun getAlignment(): Float = java.awt.Component.RIGHT_ALIGNMENT
    override fun getClickConsumer(): Consumer<MouseEvent>? = null
}
