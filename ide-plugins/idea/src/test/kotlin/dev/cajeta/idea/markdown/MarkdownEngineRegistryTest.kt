package dev.cajeta.idea.markdown

import org.junit.Assert.assertEquals
import org.junit.Assert.assertSame
import org.junit.Test

/**
 * Pure test of the markdown-engine resolution core (W4a). The registry pulls
 * the engine list from an ExtensionPointName at runtime, but the selection
 * logic — pick the engine whose id matches the settings, else fall back to the
 * first registered — is a pure function so it needs no platform fixture.
 */
class MarkdownEngineRegistryTest {

    private class FakeEngine(override val id: String) : MarkdownEngine {
        override val displayName: String = "Fake $id"
        override fun renderToHtml(markdown: String): String = markdown
    }

    @Test
    fun resolvePicksEngineMatchingSettingsId() {
        val a = FakeEngine("a")
        val b = FakeEngine("b")
        assertSame(b, MarkdownEngineRegistry.resolve(listOf(a, b), "b"))
    }

    @Test
    fun resolveFallsBackToFirstWhenIdUnknown() {
        val a = FakeEngine("a")
        val b = FakeEngine("b")
        assertSame(a, MarkdownEngineRegistry.resolve(listOf(a, b), "missing"))
    }

    @Test
    fun resolveFallsBackToFirstWhenIdNull() {
        val a = FakeEngine("a")
        val b = FakeEngine("b")
        assertSame(a, MarkdownEngineRegistry.resolve(listOf(a, b), null))
    }

    @Test
    fun resolveReturnsTheSoleEngine() {
        val only = FakeEngine("only")
        assertEquals("only", MarkdownEngineRegistry.resolve(listOf(only), "only").id)
        assertEquals("only", MarkdownEngineRegistry.resolve(listOf(only), null).id)
    }
}
