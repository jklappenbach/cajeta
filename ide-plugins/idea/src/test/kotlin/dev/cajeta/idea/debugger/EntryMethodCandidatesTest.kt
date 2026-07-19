package dev.cajeta.idea.debugger

import dev.cajeta.idea.buildtool.CajetaManifest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * run-config-ergonomics 3.1 — entry-method candidates (spec §2, §6).
 *
 * Pure merge over already-read inputs: manifest build settings plus xref
 * declaration records. No Project, no index, no EDT.
 */
class EntryMethodCandidatesTest {

    private fun rec(json: String) = dev.cajeta.idea.debugger.Json.parse(json) as Json.Obj

    private val serverMain = rec("""
        {"fqn":"mcp.Server.main","kind":"method","owner":"mcp.Server",
         "modifiers":["public","static"]}
    """.trimIndent())

    private val toolMain = rec("""
        {"fqn":"mcp.Tool.main","kind":"method","owner":"mcp.Tool",
         "modifiers":["public","static"]}
    """.trimIndent())

    // 3.1.1 / spec 2.1.2b
    @Test
    fun offersStaticMainMethodsFromTheIndex() {
        val out = EntryMethodCandidates.merge(
            CajetaManifest.BuildSettings(),
            listOf(serverMain, toolMain),
            indexAvailable = true)
        assertEquals(listOf("mcp.Server.main", "mcp.Tool.main"),
            out.candidates.map { it.fqn })
        assertTrue(out.candidates.none { it.declared })
    }

    // 3.1.2 — a non-static main, a class named main, and a non-main static are
    // all excluded.
    @Test
    fun excludesNonStaticMainClassNamedMainAndOtherMethods() {
        val nonStatic = rec("""{"fqn":"a.B.main","kind":"method","modifiers":["public"]}""")
        val classMain = rec("""{"fqn":"a.main","kind":"class","modifiers":["public"]}""")
        val otherStatic = rec("""{"fqn":"a.B.run","kind":"method","modifiers":["static"]}""")
        val out = EntryMethodCandidates.merge(
            CajetaManifest.BuildSettings(),
            listOf(nonStatic, classMain, otherStatic),
            indexAvailable = true)
        assertTrue("expected no candidates, got ${out.candidates}", out.candidates.isEmpty())
    }

    // 3.1.3 / spec 2.1.4 — declared before discovered.
    @Test
    fun manifestCandidatesComeFirstAndAreMarkedDeclared() {
        val manifest = CajetaManifest.BuildSettings(
            entryMethod = "mcp.Cli.main",
            binaries = mapOf("tool" to "mcp.Tool.main"))
        val out = EntryMethodCandidates.merge(
            manifest, listOf(serverMain), indexAvailable = true)

        assertEquals(listOf("mcp.Cli.main", "mcp.Tool.main", "mcp.Server.main"),
            out.candidates.map { it.fqn })
        assertEquals(listOf(true, true, false), out.candidates.map { it.declared })
    }

    // 3.1.3 — every binary in a MULTI-BINARY manifest is offered.
    @Test
    fun everyBinaryEntryMethodIsOffered() {
        val manifest = CajetaManifest.BuildSettings(
            binaries = mapOf("server" to "mcp.Server.main", "tool" to "mcp.Tool.main"))
        val out = EntryMethodCandidates.merge(
            manifest, emptyList(), indexAvailable = true)
        assertEquals(listOf("mcp.Server.main", "mcp.Tool.main"),
            out.candidates.map { it.fqn })
    }

    // 3.1.4 / spec 2.1.3 — same method from both sources appears once, compared
    // AFTER normalization, and keeps its declared standing.
    @Test
    fun duplicatesCollapseAfterNormalization() {
        val manifest = CajetaManifest.BuildSettings(entryMethod = "mcp.Server::main")
        val out = EntryMethodCandidates.merge(
            manifest, listOf(serverMain), indexAvailable = true)
        assertEquals(listOf("mcp.Server.main"), out.candidates.map { it.fqn })
        assertTrue("the surviving entry should stay declared",
            out.candidates.single().declared)
    }

    // 3.1.5 / spec 2.1.1, 2.2.4 — free text is accepted verbatim.
    // 3.1.6 / spec 2.1.5, 2.2.6 — what persists is always the dotted form.
    @Test
    fun freeTextIsAcceptedAndPersistsNormalized() {
        assertEquals("some.Other.main",
            EntryMethodCandidates.persistedValueFor("  some.Other.main  "))
        assertEquals("mcp.Server.main",
            EntryMethodCandidates.persistedValueFor("mcp.Server::main"))
        assertEquals("", EntryMethodCandidates.persistedValueFor("   "))
    }

    // 3.1.7 / spec 2.2.5, 6.1.2, 6.2.1 — a cold index still offers declared
    // candidates, and reports itself unavailable rather than empty.
    @Test
    fun coldIndexStillOffersManifestCandidatesAndSaysSo() {
        val manifest = CajetaManifest.BuildSettings(entryMethod = "mcp.Server::main")
        val out = EntryMethodCandidates.merge(
            manifest, emptyList(), indexAvailable = false)

        assertEquals(listOf("mcp.Server.main"), out.candidates.map { it.fqn })
        assertFalse(out.indexAvailable)
        assertTrue(out.indexUnavailable)
        assertFalse("unavailable is NOT the same as 'none found'", out.noCandidatesFound)
    }

    // 3.1.8 / spec 6.1.3 — a warm index with genuinely no main is a DIFFERENT
    // state from an unavailable one. Conflating them tells the developer their
    // project has no entry point when the truth is we never looked.
    @Test
    fun warmIndexWithNoMainIsADistinctStateFromUnavailable() {
        val out = EntryMethodCandidates.merge(
            CajetaManifest.BuildSettings(), emptyList(), indexAvailable = true)

        assertTrue(out.candidates.isEmpty())
        assertTrue(out.noCandidatesFound)
        assertFalse(out.indexUnavailable)

        val cold = EntryMethodCandidates.merge(
            CajetaManifest.BuildSettings(), emptyList(), indexAvailable = false)
        assertTrue(cold.indexUnavailable)
        assertFalse(cold.noCandidatesFound)
        assertTrue("the two empty states must not read alike",
            out.emptyMessage() != cold.emptyMessage())
    }
}
