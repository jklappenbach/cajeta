package dev.cajeta.idea.markdown

/**
 * Plain-JVM decision core for the "Toggle Markdown Rendering in Comments"
 * action (W3b). Kept free of `com.intellij.*` so it unit-tests without a
 * platform fixture — the action and the fold manager are thin delegates over
 * this. See [ToggleMarkdownRenderingAction] and
 * [MarkdownFoldEditorListener.applyRenderingState].
 */
object MarkdownRenderingToggle {

    /** What to do to a single editor when the feature reaches [enabled]. */
    enum class EditorOp { INSTALL, UNINSTALL }

    /** Render ON installs the comment fold regions; OFF tears them down. */
    fun opFor(enabled: Boolean): EditorOp =
        if (enabled) EditorOp.INSTALL else EditorOp.UNINSTALL

    /** The new state after toggling the current one. */
    fun flip(current: Boolean): Boolean = !current
}
