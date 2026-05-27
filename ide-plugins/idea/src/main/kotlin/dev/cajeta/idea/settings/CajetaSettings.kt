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
        var testFixturesPath: String = DEFAULT_FIXTURES_PATH,
        var testTypingDelayMs: Int = 30,
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

    var testFixturesPath: String
        get() = state.testFixturesPath
        set(value) { state.testFixturesPath = value }

    var testTypingDelayMs: Int
        get() = state.testTypingDelayMs
        set(value) { state.testTypingDelayMs = value }

    companion object {
        // Default points at the in-tree cajeta build. Users override
        // in Settings | Languages & Frameworks | Cajeta.
        const val DEFAULT_COMPILER_PATH = "/home/julian/code/cpp/cajeta/build/src/cajeta"
        const val DEFAULT_FIXTURES_PATH = "/home/julian/code/cpp/cajeta/ide-plugins/idea/test-code"

        val instance: CajetaSettings get() = service()
    }
}
