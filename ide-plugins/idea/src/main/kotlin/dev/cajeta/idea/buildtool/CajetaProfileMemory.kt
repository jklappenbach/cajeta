package dev.cajeta.idea.buildtool

import com.intellij.execution.RunManager
import com.intellij.openapi.components.PersistentStateComponent
import com.intellij.openapi.components.Service
import com.intellij.openapi.components.State
import com.intellij.openapi.components.Storage
import com.intellij.openapi.project.Project

/**
 * Remembers which DI profile each run/debug configuration was last set to
 * (Julian, 2026-08-02).
 *
 * One global "active profile" is wrong the moment a project has more than one
 * configuration: switching from a dev-flavoured run to a prod one silently
 * carries the previous profile across, and the build that comes out is not the
 * one the developer thinks they asked for. Keyed by configuration, switching
 * configurations restores what that configuration was using.
 *
 * A configuration with no remembered choice falls back to the first DISCOVERED
 * profile, not to a hardcoded name — see [CajetaProfileCandidates.Result.defaultSelection].
 */
@Service(Service.Level.PROJECT)
@State(
    name = "CajetaProfileMemory",
    storages = [Storage("cajeta-profiles.xml")],
)
class CajetaProfileMemory : PersistentStateComponent<CajetaProfileMemory.State> {

    class State {
        /** run/debug configuration name -> the DI profile chosen for it. */
        @JvmField
        var byConfiguration: MutableMap<String, String> = LinkedHashMap()
    }

    private var state = State()

    override fun getState(): State = state
    override fun loadState(state: State) { this.state = state }

    /** The profile remembered for [configurationName], or null. */
    fun profileFor(configurationName: String): String? =
        state.byConfiguration[configurationName]?.ifBlank { null }

    fun remember(configurationName: String, profile: String) {
        if (configurationName.isBlank()) return
        if (profile.isBlank()) state.byConfiguration.remove(configurationName)
        else state.byConfiguration[configurationName] = profile
    }

    companion object {
        fun getInstance(project: Project): CajetaProfileMemory =
            project.getService(CajetaProfileMemory::class.java)

        /**
         * The configuration the toolbar's selector currently speaks for: the
         * one selected in the run/debug dropdown. Empty when nothing is
         * selected, in which case the choice is not remembered per
         * configuration and the global default still applies.
         */
        fun currentConfigurationName(project: Project): String =
            try {
                RunManager.getInstance(project).selectedConfiguration?.name ?: ""
            } catch (_: Throwable) {
                ""
            }
    }
}
