package dev.cajeta.idea.jsonl

import com.intellij.openapi.components.PersistentStateComponent
import com.intellij.openapi.components.Service
import com.intellij.openapi.components.State
import com.intellij.openapi.components.Storage
import com.intellij.openapi.project.Project

/**
 * Remembers each run/debug profile's console table layout across sessions
 * (json-viewer spec §3.1.9). Project-level, because the profiles are: the same
 * configuration name in another project is another console.
 *
 * The store deliberately knows nothing about columns — it maps a profile key
 * to an opaque encoded [JsonlColumnLayout]. Every decision about what a layout
 * means lives in the pure model, which is where it can be tested.
 */
@Service(Service.Level.PROJECT)
@State(
    name = "CajetaJsonConsoleLayouts",
    storages = [Storage("cajeta-json-console.xml")],
)
class JsonConsoleLayoutStore : PersistentStateComponent<JsonConsoleLayoutStore.State> {

    /** Serialized form: profile key -> encoded layout. A plain map keeps the
     *  XML readable and lets a stale entry be deleted by hand. */
    class State {
        @JvmField
        var layouts: MutableMap<String, String> = LinkedHashMap()
    }

    private var state = State()

    override fun getState(): State = state
    override fun loadState(state: State) { this.state = state }

    /** The layout saved for [profileKey], or null if there is none or the
     *  stored one is unreadable (§3.1.9.5). */
    fun load(profileKey: String): JsonlColumnLayout? {
        if (profileKey.isBlank()) return null
        return JsonlColumnLayout.decode(state.layouts[profileKey])
    }

    fun save(profileKey: String, layout: JsonlColumnLayout) {
        if (profileKey.isBlank()) return
        state.layouts[profileKey] = layout.encode()
    }

    companion object {
        fun getInstance(project: Project): JsonConsoleLayoutStore =
            project.getService(JsonConsoleLayoutStore::class.java)

        /**
         * The key a console persists under (§3.1.9.1): the configuration's
         * own name, namespaced by kind so a build console and a debug session
         * of the same configuration keep separate layouts — they carry
         * different record shapes and want different columns.
         */
        fun keyFor(kind: String, configurationName: String): String =
            if (configurationName.isBlank()) "" else "$kind/$configurationName"
    }
}
