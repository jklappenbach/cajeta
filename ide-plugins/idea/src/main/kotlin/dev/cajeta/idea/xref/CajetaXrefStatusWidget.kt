package dev.cajeta.idea.xref

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.application.ModalityState
import com.intellij.openapi.project.Project
import com.intellij.openapi.util.Disposer
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
    // Must actually dispose: the widget unsubscribes from the freshness
    // service there, and an empty override leaks one listener per project close.
    override fun disposeWidget(widget: StatusBarWidget) = Disposer.dispose(widget)
    override fun canBeEnabledOn(statusBar: StatusBar): Boolean = true

    companion object {
        const val WIDGET_ID = "CajetaXrefStatus"
    }
}

private class CajetaXrefStatusWidget(private val project: Project) :
    StatusBarWidget, StatusBarWidget.TextPresentation {

    private var statusBar: StatusBar? = null

    /**
     * A StatusBarWidget's getText() is PULLED — the platform calls it when it
     * repaints, and nothing repaints on its own. Without this subscription the
     * bar keeps showing whatever it last drew, which is how it came to read
     * "stale" over an index that had rebuilt successfully (Julian, live
     * 2026-08-24). The state machine was right; the pixels were old.
     */
    private val onFreshnessChanged: () -> Unit = {
        val bar = statusBar
        if (bar != null) {
            // Freshness flips from a background rebuild thread; touching the
            // status bar is EDT work. `any()` so the update still lands while a
            // modal dialog is up rather than queueing behind it.
            ApplicationManager.getApplication().invokeLater(
                { if (!project.isDisposed) bar.updateWidget(ID()) },
                ModalityState.any(),
            )
        }
    }

    override fun ID(): String = CajetaXrefStatusWidgetFactory.WIDGET_ID
    override fun getPresentation(): StatusBarWidget.WidgetPresentation = this

    override fun install(statusBar: StatusBar) {
        this.statusBar = statusBar
        CajetaXrefFreshness.getInstance(project).addChangeListener(onFreshnessChanged)
    }

    override fun dispose() {
        CajetaXrefFreshness.getInstance(project).removeChangeListener(onFreshnessChanged)
        statusBar = null
    }

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
