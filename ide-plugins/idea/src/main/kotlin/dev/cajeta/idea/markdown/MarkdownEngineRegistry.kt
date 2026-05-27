package dev.cajeta.idea.markdown

import com.intellij.openapi.components.Service
import com.intellij.openapi.components.service
import dev.cajeta.idea.markdown.engines.JetBrainsMarkdownEngine
import dev.cajeta.idea.settings.CajetaSettings

/**
 * App-level service that resolves the configured markdown engine.
 * Hard-coded engine list for v0.1; promote to an ExtensionPointName
 * if third-party engines need to plug in later.
 */
@Service(Service.Level.APP)
class MarkdownEngineRegistry {

    private val engines: Map<String, MarkdownEngine> = listOf(
        JetBrainsMarkdownEngine(),
    ).associateBy { it.id }

    fun active(): MarkdownEngine {
        val id = CajetaSettings.instance.markdownEngineId
        return engines[id] ?: engines.values.first()
    }

    fun all(): Collection<MarkdownEngine> = engines.values

    companion object {
        fun getInstance(): MarkdownEngineRegistry = service()
    }
}
