package dev.cajeta.idea.xref

import com.intellij.psi.PsiElement
import com.intellij.psi.PsiPolyVariantReference
import com.intellij.testFramework.fixtures.BasePlatformTestCase
import dev.cajeta.idea.lint.XrefStream
import dev.cajeta.idea.lint.XrefStreamParser

/**
 * ide-symbol-index Unit 9 (plan 9.1.1 - 9.1.6): the price of compiler-
 * authoritative resolution, paid honestly. A STALE answer must never become a
 * WRONG one: the recorded offset is followed only if what sits there still
 * matches the recorded declaration; degradation is visible and reasoned, never
 * silence.
 */
class CajetaXrefFreshnessTest : BasePlatformTestCase() {

    private fun line(rel: String, record: String) =
        """{"kind":"xref","rel":"$rel","record":$record}"""

    private val version = line("version", """{"major": 1, "minor": 0}""")

    private fun refAt(file: com.intellij.psi.PsiFile, needle: String): PsiPolyVariantReference? {
        val at = file.text.indexOf(needle)
        var e: PsiElement? = file.findElementAt(at)
        while (e != null && e.reference == null) e = e.parent
        return e?.reference as? PsiPolyVariantReference
    }

    // ---- 9.1.1 — the previous export stays queryable during a refresh -------------

    fun testPreviousShardsStayQueryableWhileARefreshIsInFlight() {
        myFixture.addFileToProject("${CajetaXrefShards.DIR}/demo_A.cjxref",
            listOf(version, line("declarations",
                """{"fqn": "demo.A", "kind": "class", "file": "demo/A.cajeta", "line": 2, "col": 13}""")
            ).joinToString("\n"))

        val freshness = CajetaXrefFreshness.getInstance(project)
        freshness.refreshStarted()
        assertEquals(CajetaXrefFreshness.State.REFRESHING, freshness.state)
        // Navigation answers from the last known export — not nothing.
        assertEquals(1, XrefQuery.declarationsOf(project, "demo.A").size)
        freshness.refreshSucceeded()
        assertEquals(CajetaXrefFreshness.State.FRESH, freshness.state)
    }

    // ---- 9.1.2 — THE load-bearing test: moved declaration is DISCARDED -------------

    fun testAMovedDeclarationIsDiscardedNotFollowedToWhateverSitsThere() {
        // The export recorded Target at line 2. The buffer has since gained a
        // line: line 2 col 13 now holds a DIFFERENT class (Imposter). Following
        // the offset would jump confidently to the wrong declaration — the one
        // failure mode this whole design exists to prevent.
        val text = "package demo;\npublic class Imposter {\n}\npublic class Target {\n}\n"
        myFixture.addFileToProject("demo/Target.cajeta", text)
        val useText = "package demo;\npublic class Use {\n    Target t;\n}\n"
        val use = myFixture.addFileToProject("demo/Use.cajeta", useText)

        val (ul, uc) = 3 to useText.split('\n')[2].indexOf("Target")
        myFixture.addFileToProject("${CajetaXrefShards.DIR}/demo_Target.cjxref",
            listOf(version, line("declarations",
                """{"fqn": "demo.Target", "kind": "class", "file": "demo/Target.cajeta", "line": 2, "col": 13}""")
            ).joinToString("\n"))
        myFixture.addFileToProject("${CajetaXrefShards.DIR}/demo_Use.cjxref",
            listOf(version, line("references",
                """{"target": "demo.Target", "kind": "type", "file": "demo/Use.cajeta", "line": $ul, "col": $uc}""")
            ).joinToString("\n"))

        // Line 2 col 13 exists and holds a named element — the WRONG one.
        val resolved = refAt(use, "Target t")!!.resolve()
        assertNull("a stale offset must be discarded, not followed to " +
            "whatever occupies it", resolved)
    }

    fun testAnUnmovedDeclarationStillValidatesAndResolves() {
        // The twin control: same setup, offset still true → resolves.
        val text = "package demo;\npublic class Target {\n}\n"
        myFixture.addFileToProject("demo/Target.cajeta", text)
        val useText = "package demo;\npublic class Use {\n    Target t;\n}\n"
        val use = myFixture.addFileToProject("demo/Use.cajeta", useText)

        val (ul, uc) = 3 to useText.split('\n')[2].indexOf("Target")
        myFixture.addFileToProject("${CajetaXrefShards.DIR}/demo_Target.cjxref",
            listOf(version, line("declarations",
                """{"fqn": "demo.Target", "kind": "class", "file": "demo/Target.cajeta", "line": 2, "col": 13}""")
            ).joinToString("\n"))
        myFixture.addFileToProject("${CajetaXrefShards.DIR}/demo_Use.cjxref",
            listOf(version, line("references",
                """{"target": "demo.Target", "kind": "type", "file": "demo/Use.cajeta", "line": $ul, "col": $uc}""")
            ).joinToString("\n"))

        assertNotNull(refAt(use, "Target t")!!.resolve())
    }

    // ---- 9.1.3 / 9.1.4 / 9.1.6 — visible, reasoned degradation ---------------------

    fun testNoCompilerReportsUnavailableWithAReason() {
        val freshness = CajetaXrefFreshness.getInstance(project)
        freshness.updateFromLint(compilerConfigured = false, stream = XrefStream.EMPTY)
        assertEquals(CajetaXrefFreshness.State.UNAVAILABLE, freshness.state)
        assertTrue(freshness.reason!!.contains("compiler", ignoreCase = true))
    }

    fun testUnknownSchemaMajorReportsUnavailableWithAReason() {
        val stream = XrefStreamParser.demux(listOf(
            """{"kind":"xref","rel":"version","record":{"major": 99, "minor": 0}}""",
            """{"kind":"xref","rel":"declarations","record":{"fqn": "x", "kind": "class", "file": "a.cajeta", "line": 1, "col": 0}}""",
        ).joinToString("\n"))
        val freshness = CajetaXrefFreshness.getInstance(project)
        freshness.updateFromLint(compilerConfigured = true, stream = stream)
        assertEquals(CajetaXrefFreshness.State.UNAVAILABLE, freshness.state)
        assertTrue(freshness.reason!!.contains("99"))
    }

    fun testASupportedStreamRestoresFreshAndAFailedRefreshGoesStale() {
        val freshness = CajetaXrefFreshness.getInstance(project)
        freshness.updateFromLint(compilerConfigured = true,
            stream = XrefStreamParser.demux(version))
        assertEquals(CajetaXrefFreshness.State.FRESH, freshness.state)

        freshness.refreshStarted()
        freshness.refreshFailed("compiler exited 1")
        // 9.1.6 — queryable staleness, so ide-features can refuse to refactor.
        assertEquals(CajetaXrefFreshness.State.STALE, freshness.state)
        assertEquals("compiler exited 1", freshness.reason)
    }
}
