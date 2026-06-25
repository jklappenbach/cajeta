package dev.cajeta.idea.markdown

import com.intellij.openapi.actionSystem.ActionUpdateThread
import com.intellij.openapi.actionSystem.AnActionEvent
import com.intellij.openapi.actionSystem.ToggleAction
import com.intellij.openapi.project.DumbAware
import dev.cajeta.idea.settings.CajetaSettings

/**
 * "Cajeta | Render Markdown in Comments" — the global toggle-action escape
 * hatch from the markdown tier (W3b; the Settings checkbox shipped first, this
 * adds the menu action). A checkable item whose state mirrors
 * [CajetaSettings.renderMarkdownInComments]; flipping it disables the feature
 * project-wide and reverts every open Cajeta comment to raw source (or
 * re-renders), via [MarkdownFoldEditorListener.applyRenderingState].
 */
class ToggleMarkdownRenderingAction : ToggleAction(), DumbAware {

    override fun getActionUpdateThread(): ActionUpdateThread = ActionUpdateThread.BGT

    override fun isSelected(e: AnActionEvent): Boolean =
        CajetaSettings.instance.renderMarkdownInComments

    override fun setSelected(e: AnActionEvent, state: Boolean) {
        CajetaSettings.instance.renderMarkdownInComments = state
        MarkdownFoldEditorListener.applyRenderingState()
    }
}
