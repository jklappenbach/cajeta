package dev.cajeta.idea.markdown

import com.intellij.openapi.components.Service
import com.intellij.openapi.components.service
import com.intellij.openapi.extensions.ExtensionPointName
import dev.cajeta.idea.markdown.engines.JetBrainsMarkdownEngine
import dev.cajeta.idea.settings.CajetaSettings

/**
 * App-level service that resolves the configured markdown engine.
 *
 * Engines register declaratively through the `dev.cajeta.idea.markdownEngine`
 * extension point (W4a), so a third-party plugin can add its own engine from
 * its own `plugin.xml` without touching this code. The built-in
 * [JetBrainsMarkdownEngine] is registered the same way in our `plugin.xml`; if
 * the extension point yields nothing (e.g. a bare unit-test classpath with no
 * platform), we fall back to it so callers always get a working engine.
 */
@Service(Service.Level.APP)
class MarkdownEngineRegistry {

    /** Registered engines, or the built-in fallback if the EP is empty. */
    private fun engines(): List<MarkdownEngine> =
        EP_NAME.extensionList.ifEmpty { listOf(JetBrainsMarkdownEngine()) }

    fun active(): MarkdownEngine =
        resolve(engines(), CajetaSettings.instance.markdownEngineId)

    fun all(): Collection<MarkdownEngine> = engines()

    companion object {
        /**
         * Extension point third-party engines plug into. Declared in
         * `plugin.xml`; `dynamic="true"` so engines can load/unload without an
         * IDE restart.
         */
        val EP_NAME: ExtensionPointName<MarkdownEngine> =
            ExtensionPointName.create("dev.cajeta.idea.markdownEngine")

        fun getInstance(): MarkdownEngineRegistry = service()

        /**
         * Pure selection core: the engine whose [MarkdownEngine.id] matches
         * [settingsId], else the first registered engine. [engines] must be
         * non-empty (the service guarantees this via the built-in fallback).
         */
        fun resolve(engines: List<MarkdownEngine>, settingsId: String?): MarkdownEngine =
            engines.firstOrNull { it.id == settingsId } ?: engines.first()
    }
}
