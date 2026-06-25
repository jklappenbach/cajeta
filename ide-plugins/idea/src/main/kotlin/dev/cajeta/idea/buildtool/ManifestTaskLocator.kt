package dev.cajeta.idea.buildtool

/**
 * Locates a task's declaration in the raw `cajeta.json` text so "Open in
 * manifest" can put the caret on it (spec §4 / plan 5.2.3). The discovery model
 * carries no source ranges, so this is a best-effort textual locate: find the
 * `"tasks"` block, then the task's key within it. Pure — no `com.intellij.*`.
 */
object ManifestTaskLocator {

    /** Offset of the opening quote of `"<taskName>"` inside the tasks block, or
     *  null if the tasks block or the task key isn't found. */
    fun offsetOf(manifestText: String, taskName: String): Int? {
        val tasksKey = Regex("\"tasks\"\\s*:").find(manifestText) ?: return null
        val key = Regex("\"" + Regex.escape(taskName) + "\"\\s*:")
            .find(manifestText, tasksKey.range.last)
            ?: return null
        return key.range.first
    }
}
