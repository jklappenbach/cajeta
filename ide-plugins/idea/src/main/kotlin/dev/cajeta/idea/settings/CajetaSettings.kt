package dev.cajeta.idea.settings

import com.intellij.openapi.components.PersistentStateComponent
import com.intellij.openapi.components.Service
import com.intellij.openapi.components.State
import com.intellij.openapi.components.Storage
import com.intellij.openapi.components.service
import com.intellij.util.xmlb.XmlSerializerUtil

@Service(Service.Level.APP)
@State(name = "CajetaSettings", storages = [Storage("cajeta.xml")])
class CajetaSettings : PersistentStateComponent<CajetaSettings.State> {

    data class State(
        var compilerPath: String = DEFAULT_COMPILER_PATH,
        var markdownEngineId: String = "jetbrains-markdown",
        var renderMarkdownInComments: Boolean = true,
        // Rendering surface for in-comment markdown: "swing" (HTMLEditorKit,
        // default, always available) or "jcef" (Chromium off-screen, full CSS
        // fidelity, experimental — falls back to swing when JCEF is unsupported).
        var markdownRenderSurface: String = MARKDOWN_SURFACE_SWING,
        var testFixturesPath: String = DEFAULT_FIXTURES_PATH,
        var testTypingDelayMs: Int = 30,
        // CP7-5 (FR-7.3): independent toggles for the three memory-facet
        // visualization surfaces. All on by default (the FR-5.2 out-of-box
        // encoding); each can be silenced without affecting the others.
        var showFacetsInVariables: Boolean = true,
        var showFacetsInGutter: Boolean = true,
        var showFacetsInline: Boolean = true,
        // Build-tool tool window (spec §14). buildToolPath defaults to `cajeta`
        // on PATH; buildAutoReload is prompt|always|never; the JSONL-view and
        // profile/flavor defaults seed new runs and newly opened views.
        var buildToolPath: String = DEFAULT_BUILD_TOOL_PATH,
        var buildAutoReload: String = AUTO_RELOAD_PROMPT,
        var defaultProfile: String = "",
        var defaultFlavor: String = "",
        var jsonlDefaultStructured: Boolean = true,
        var jsonlDefaultLevel: String = "",
        // Tool-window toolbar: group the tree by project root vs. flat. Persisted
        // so the grouping toggle survives restarts (spec §9.2.4).
        var buildGroupByProject: Boolean = true,
        // Route build-family tasks (compile/package/… and non-runnable tasks) to
        // the native Build tool window instead of the Run window (build-toolwindow
        // spec §6). Escape hatch: off restores the old Run-window behaviour.
        var buildTasksInBuildWindow: Boolean = true,
        // Route per-edit lint through the warm `cajeta --lint-server` daemon
        // (lint-server-spec §5). On by default: the daemon primes the stdlib once,
        // so warm lints are sub-second instead of the ~19s cold one-shot compile.
        // Off falls back to a fresh one-shot `cajeta --lint` per edit.
        var useLintServer: Boolean = true,
    )

    private var state = State()

    override fun getState(): State = state

    override fun loadState(loaded: State) {
        XmlSerializerUtil.copyBean(loaded, state)
    }

    var compilerPath: String
        get() = state.compilerPath
        set(value) { state.compilerPath = value }

    var markdownEngineId: String
        get() = state.markdownEngineId
        set(value) { state.markdownEngineId = value }

    var renderMarkdownInComments: Boolean
        get() = state.renderMarkdownInComments
        set(value) { state.renderMarkdownInComments = value }

    var markdownRenderSurface: String
        get() = state.markdownRenderSurface
        set(value) { state.markdownRenderSurface = value }

    var testFixturesPath: String
        get() = state.testFixturesPath
        set(value) { state.testFixturesPath = value }

    var testTypingDelayMs: Int
        get() = state.testTypingDelayMs
        set(value) { state.testTypingDelayMs = value }

    var showFacetsInVariables: Boolean
        get() = state.showFacetsInVariables
        set(value) { state.showFacetsInVariables = value }

    var showFacetsInGutter: Boolean
        get() = state.showFacetsInGutter
        set(value) { state.showFacetsInGutter = value }

    var showFacetsInline: Boolean
        get() = state.showFacetsInline
        set(value) { state.showFacetsInline = value }

    var buildToolPath: String
        get() = state.buildToolPath
        set(value) { state.buildToolPath = value }

    var buildAutoReload: String
        get() = state.buildAutoReload
        set(value) { state.buildAutoReload = value }

    var defaultProfile: String
        get() = state.defaultProfile
        set(value) { state.defaultProfile = value }

    var defaultFlavor: String
        get() = state.defaultFlavor
        set(value) { state.defaultFlavor = value }

    var jsonlDefaultStructured: Boolean
        get() = state.jsonlDefaultStructured
        set(value) { state.jsonlDefaultStructured = value }

    var jsonlDefaultLevel: String
        get() = state.jsonlDefaultLevel
        set(value) { state.jsonlDefaultLevel = value }

    var buildGroupByProject: Boolean
        get() = state.buildGroupByProject
        set(value) { state.buildGroupByProject = value }

    var buildTasksInBuildWindow: Boolean
        get() = state.buildTasksInBuildWindow
        set(value) { state.buildTasksInBuildWindow = value }

    var useLintServer: Boolean
        get() = state.useLintServer
        set(value) { state.useLintServer = value }

    companion object {
        // Default points at the in-tree cajeta build. Users override
        // in Settings | Languages & Frameworks | Cajeta.
        const val DEFAULT_COMPILER_PATH = "/home/julian/code/cpp/cajeta/build/src/cajeta"
        const val DEFAULT_FIXTURES_PATH = "/home/julian/code/cpp/cajeta/ide-plugins/idea/test-code"

        // Build tool resolved on PATH by default (spec §14.1).
        const val DEFAULT_BUILD_TOOL_PATH = "cajeta"
        const val AUTO_RELOAD_PROMPT = "prompt"
        const val AUTO_RELOAD_ALWAYS = "always"
        const val AUTO_RELOAD_NEVER = "never"

        // In-comment markdown rendering surfaces (spec: markdown JCEF prototype).
        const val MARKDOWN_SURFACE_SWING = "swing"
        const val MARKDOWN_SURFACE_JCEF = "jcef"

        val instance: CajetaSettings get() = service()
    }
}
