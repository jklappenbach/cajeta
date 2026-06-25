package dev.cajeta.idea.buildtool

import com.intellij.openapi.components.PersistentStateComponent
import com.intellij.openapi.components.Service
import com.intellij.openapi.components.State
import com.intellij.openapi.components.Storage
import com.intellij.openapi.components.service
import com.intellij.openapi.project.Project

/**
 * Persists the project's favorite tasks (spec §11.2.3), surfaced pinned at the
 * top of the tool window. Delegates set semantics to the pure [Favorites]; this
 * is the thin persistent shell.
 */
@Service(Service.Level.PROJECT)
@State(name = "CajetaBuildFavorites", storages = [Storage("cajeta-favorites.xml")])
class FavoritesService : PersistentStateComponent<FavoritesService.State> {

    /** Serializable favorite entry (no-arg ctor for XmlSerializer). */
    class Entry {
        var manifestPath: String = ""
        var task: String = ""
    }

    class State {
        var favorites: MutableList<Entry> = mutableListOf()
    }

    private var state = State()

    override fun getState(): State = state
    override fun loadState(loaded: State) { state = loaded }

    fun list(): List<FavoriteRef> = state.favorites.map { FavoriteRef(it.manifestPath, it.task) }

    fun contains(ref: FavoriteRef): Boolean = Favorites(list()).contains(ref)

    fun toggle(ref: FavoriteRef) {
        state.favorites = Favorites(list()).toggle(ref).refs
            .map { Entry().apply { manifestPath = it.manifestPath; task = it.task } }
            .toMutableList()
    }

    companion object {
        fun getInstance(project: Project): FavoritesService = project.service()
    }
}
