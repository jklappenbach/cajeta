package dev.cajeta.idea.buildtool

/**
 * The pure enable/disable + grouping state for the tool-window toolbar (spec
 * §9). All decision logic lives here so the platform `ActionToolbar` actions are
 * thin `update()` delegates: Stop lights only while runs are active (§9.2.3),
 * Run needs a runnable selection, the picker / expand-collapse need a populated
 * tree, and the grouping toggle (§9.2.4) is a value flip the caller persists. No
 * `com.intellij.*`.
 */
data class BuildToolbarState(
    val hasTasks: Boolean = false,
    val activeRuns: Int = 0,
    val selectionRunnable: Boolean = false,
    val groupByProject: Boolean = true,
) {
    val refreshEnabled: Boolean get() = true
    val stopEnabled: Boolean get() = activeRuns > 0
    val runSelectedEnabled: Boolean get() = selectionRunnable
    val runTaskPickerEnabled: Boolean get() = hasTasks
    val expandCollapseEnabled: Boolean get() = hasTasks

    /** The state with the grouping mode flipped (caller persists the new value). */
    fun toggledGrouping(): BuildToolbarState = copy(groupByProject = !groupByProject)
}
