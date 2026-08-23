package dev.cajeta.idea.coverage

import com.intellij.testFramework.fixtures.BasePlatformTestCase

/**
 * The Dead Code tab's context actions, on real PSI.
 *
 * Written 2026-08-23 because Safe Delete CORRUPTED SOURCE in front of the
 * developer. It removed a method and left its modifiers:
 *
 * ```cajeta
 * /** The pre-2.0 tier ladder. */
 * public static
 * ```
 *
 * The file stopped parsing. The cause is in the grammar and needs no IDE to
 * see — `classBodyDeclaration : modifier* memberDeclaration`, so the named
 * element for a method BEGINS AFTER the modifiers — which makes this exactly
 * the kind of defect a unit test catches and 209 existing ones did not,
 * because none of them exercised what the actions resolve to.
 *
 * Safe Delete's own usage search cannot save us here: it validates WHETHER to
 * delete, never WHAT. A half-declaration passes every conflict check.
 */
class CocoContextActionsTest : BasePlatformTestCase() {

    private val source = """
        package tour.coco;

        public class LegacyPricing {

            /** The pre-2.0 tier ladder. */
            public static int64 tierPrice(int64 tier, int64 baseCents) {
                if (tier <= 1) {
                    return baseCents;
                }
                return (baseCents * 75) / 100;
            }

            /** Nothing calls this. */
            public static int64 legacyRound(int64 cents) {
                return cents - (cents % 5);
            }
        }
    """.trimIndent()

    private fun finding(line: Int, verdict: Verdict = Verdict.DELETION_CANDIDATE) =
        UncoveredMethod(
            key = "tour.coco.LegacyPricing::tierPrice(int64,int64)",
            owner = "tour.coco.LegacyPricing",
            method = "tierPrice",
            file = "LegacyPricing.cajeta",
            line = line,
            verdict = verdict,
            reason = "unreachable",
        )

    /** 1-based line of `public static int64 tierPrice`. */
    private fun tierPriceLine(): Int =
        source.lines().indexOfFirst { it.contains("int64 tierPrice") } + 1

    // ── Safe Delete ────────────────────────────────────────────────────────

    fun `test the delete target covers the modifiers`() {
        val psi = myFixture.configureByText("LegacyPricing.cajeta", source)
        val action = CocoSafeDeleteAction { finding(tierPriceLine()) }

        val element = action.elementAt(psi, tierPriceLine())
        assertNotNull("no element resolved for the finding", element)

        val text = element!!.text.trim()
        assertTrue(
            "the delete target must START at the modifiers, or deleting it " +
                "strands them and the file stops parsing. Got: <$text>",
            text.startsWith("public static"),
        )
        assertTrue("the delete target must include the method body", text.endsWith("}"))
        assertTrue("the delete target must be the right method", text.contains("tierPrice"))
        assertFalse(
            "the delete target swallowed the following declaration",
            text.contains("legacyRound"),
        )
    }

    /**
     * The end-to-end property, stated as the developer experienced it: after
     * removing the element, no orphaned modifier is left behind.
     */
    fun `test removing the target leaves no dangling modifiers`() {
        val psi = myFixture.configureByText("LegacyPricing.cajeta", source)
        val action = CocoSafeDeleteAction { finding(tierPriceLine()) }
        val element = action.elementAt(psi, tierPriceLine())!!

        val remaining = source.replace(element.text, "")
        assertFalse(
            "a bare `public static` survived the delete — this is the exact " +
                "corruption the action shipped with:\n$remaining",
            Regex("""(?m)^\s*(public|private|protected)?\s*static\s*$""")
                .containsMatchIn(remaining),
        )
        assertTrue("the untouched method must survive", remaining.contains("legacyRound"))
        assertFalse("the deleted method must be gone", remaining.contains("tierPrice"))
    }

    fun `test delete is offered only for deletion candidates`() {
        myFixture.configureByText("LegacyPricing.cajeta", source)
        for (v in Verdict.entries) {
            val action = CocoSafeDeleteAction { finding(tierPriceLine(), v) }
            val e = com.intellij.testFramework.TestActionEvent.createTestEvent(action)
            action.update(e)
            assertEquals(
                "Safe Delete must be enabled for DELETION_CANDIDATE and nothing " +
                    "else — offering it on a needs-a-test row invites deleting " +
                    "the very code someone should be testing (verdict=$v)",
                v == Verdict.DELETION_CANDIDATE,
                e.presentation.isEnabled,
            )
        }
    }

    fun `test delete resolves nothing when the file cannot be found`() {
        myFixture.configureByText("LegacyPricing.cajeta", source)
        val missing = UncoveredMethod(
            key = "x.Y::z()", owner = "x.Y", method = "z",
            file = "no/such/File.cajeta", line = 3,
            verdict = Verdict.DELETION_CANDIDATE, reason = "unreachable",
        )
        val action = CocoSafeDeleteAction { missing }
        assertNull(
            "an unresolvable finding must resolve to NO element; deleting the " +
                "nearest thing to hand is how a refactoring eats the wrong file",
            action.elementFor(project, missing),
        )
    }

    // ── Jump to Source ─────────────────────────────────────────────────────

    fun `test jump to source resolves an absolute path to a real file`() {
        val dir = java.nio.file.Files.createTempDirectory("coco-nav").toFile()
        val f = java.io.File(dir, "LegacyPricing.cajeta").apply { writeText(source) }
        com.intellij.openapi.vfs.LocalFileSystem.getInstance().refreshAndFindFileByIoFile(f)

        val vf = CocoNavigation.resolve(project, f.absolutePath)
        assertNotNull("an absolute path to an existing file must resolve", vf)
        assertEquals(f.name, vf!!.name)
    }

    fun `test jump to source refuses an unresolvable path`() {
        myFixture.configureByText("LegacyPricing.cajeta", source)
        assertFalse(
            "navigation must refuse rather than open some other file",
            CocoNavigation.open(project, "no/such/File.cajeta", 1),
        )
    }
}
