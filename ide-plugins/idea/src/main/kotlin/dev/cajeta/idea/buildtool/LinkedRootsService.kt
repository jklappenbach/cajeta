package dev.cajeta.idea.buildtool

import com.intellij.openapi.components.PersistentStateComponent
import com.intellij.openapi.components.Service
import com.intellij.openapi.components.State
import com.intellij.openapi.components.Storage
import com.intellij.openapi.components.service
import com.intellij.openapi.project.Project

/**
 * Persists the manually linked `cajeta.json` roots for a project (spec §10.2.2/3)
 * — separate from the auto-detected project root. Delegates the set semantics to
 * the pure [LinkedRoots]; this is just the thin persistent shell.
 */
@Service(Service.Level.PROJECT)
@State(name = "CajetaLinkedRoots", storages = [Storage("cajeta-roots.xml")])
class LinkedRootsService : PersistentStateComponent<LinkedRootsService.State> {

    class State {
        var roots: MutableList<String> = mutableListOf()
    }

    private var state = State()

    override fun getState(): State = state
    override fun loadState(loaded: State) { state = loaded }

    fun linkedPaths(): List<String> = LinkedRoots(state.roots.toList()).paths

    fun link(path: String) { state.roots = LinkedRoots(state.roots.toList()).link(path).paths.toMutableList() }

    fun unlink(path: String) { state.roots = LinkedRoots(state.roots.toList()).unlink(path).paths.toMutableList() }

    companion object {
        fun getInstance(project: Project): LinkedRootsService = project.service()
    }
}
