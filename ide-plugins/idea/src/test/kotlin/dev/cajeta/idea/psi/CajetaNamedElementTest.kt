package dev.cajeta.idea.psi

import com.intellij.psi.PsiElement
import com.intellij.psi.util.PsiTreeUtil
import com.intellij.testFramework.fixtures.BasePlatformTestCase
import dev.cajeta.idea.editor.CajetaStructureViewFactory
import dev.cajeta.idea.parser.CajetaPsiFile

/**
 * ide-symbol-index Unit 5 (plan 5.1.1 - 5.1.5): named PSI elements. Pure syntax —
 * the IDE owns naming and nothing beyond it (spec §3). Each declaring rule yields
 * a typed element implementing PsiNameIdentifierOwner instead of a blanket
 * ANTLRPsiNode, with the name identifier's range covering exactly the identifier
 * token, and the FQN/offset the xref export keys on (§3.0.4).
 *
 * The multi-declarator subtlety: `int32 count, backup;` declares TWO names, and a
 * single owner whose nameIdentifier is `count` would make Rename on `backup`
 * silently target `count` — a wrong answer. So the per-name owner is the
 * variableDeclaratorId element; the field/local declaration delegates when it
 * declares exactly one name and declines (null) otherwise.
 */
class CajetaNamedElementTest : BasePlatformTestCase() {

    private val source = """
        package demo.app;

        public class Outer<T> {
            int32 count, backup;
            public Outer(int32 seed) {
                this.count = seed;
                return;
            }
            public int32 bump(int32 delta) {
                int32 next = delta;
                return next;
            }
            public Outer operator+(Outer other) {
                return this;
            }
            class Inner {
            }
        }

        interface Greeter {
            void greet(String who);
        }

        enum Color { RED, GREEN }

        annotation Retry {
            int32 attempts() default 3;
        }
    """.trimIndent()

    private fun file(): CajetaPsiFile {
        myFixture.configureByText("Outer.cajeta", source)
        return myFixture.file as CajetaPsiFile
    }

    private fun named(file: CajetaPsiFile): List<CajetaNamedElement> =
        PsiTreeUtil.collectElementsOfType(file, CajetaNamedElement::class.java).toList()

    private fun find(file: CajetaPsiFile, name: String): CajetaNamedElement =
        named(file).firstOrNull { it.name == name }
            ?: throw AssertionError("no named element '$name'; have: " +
                named(file).map { it.name })

    // ---- 5.1.1 — typed elements for every declaring rule ----------------------

    fun testEveryDeclaringRuleYieldsATypedNamedElement() {
        val names = named(file()).map { it.name }
        // class, interface, enum, annotation, nested class
        for (expected in listOf("Outer", "Inner", "Greeter", "Color", "Retry",
                                // method, constructor, interface method,
                                // annotation element
                                "bump", "Outer", "greet", "attempts",
                                // fields (per-name), local, parameter, type
                                // parameter, enum constants
                                "count", "backup", "next", "delta", "seed",
                                "who", "T", "RED", "GREEN")) {
            assertTrue("missing named element '$expected' in $names",
                names.contains(expected))
        }
    }

    // ---- 5.1.2 — name + identifier range ---------------------------------------

    fun testNameIdentifierCoversExactlyTheIdentifierToken() {
        val f = file()
        for (name in listOf("Outer", "Greeter", "bump", "count", "backup",
                            "delta", "T", "RED", "Retry")) {
            val e = find(f, name)
            val id = e.nameIdentifier
            assertNotNull("$name has no nameIdentifier", id)
            assertEquals("$name identifier text", name, id!!.text)
            assertEquals("$name identifier range must cover exactly the token",
                name.length, id.textRange.length)
        }
    }

    fun testMultiDeclaratorFieldNamesAreSeparatelyOwned() {
        val f = file()
        val count = find(f, "count")
        val backup = find(f, "backup")
        // Different owners at different offsets — Rename on `backup` must not
        // target `count`.
        assertTrue(count.nameIdentifier!!.textRange.startOffset
            < backup.nameIdentifier!!.textRange.startOffset)
        assertNotSame(count, backup)
    }

    // ---- 5.1.3 — setName -------------------------------------------------------

    fun testSetNameRewritesTheIdentifierAndNothingElse() {
        val f = file()
        val before = f.text
        com.intellij.openapi.command.WriteCommandAction.runWriteCommandAction(project) {
            find(f, "bump").setName("nudge")
        }
        val after = f.text
        assertEquals(before.replaceFirst("int32 bump(", "int32 nudge("), after)
        assertNotNull(find(f, "nudge"))
    }

    // ---- 5.1.4 — FQNs the export keys on ----------------------------------------

    fun testFqnsMatchTheExportsKeys() {
        val f = file()
        assertEquals("demo.app.Outer", find(f, "Outer").fqn)
        assertEquals("demo.app.Outer.Inner", find(f, "Inner").fqn)
        assertEquals("demo.app.Outer.bump", find(f, "bump").fqn)
        assertEquals("demo.app.Color", find(f, "Color").fqn)
        // The compiler canonicalizes EVERY annotation under the synthetic
        // "code" package (visitAnnotationTypeDeclaration) — the export says
        // code.Retry, so the PSI element must too, or the two can never meet.
        assertEquals("code.Retry", find(f, "Retry").fqn)
    }

    // ---- 5.1.5 — operator declarations are named --------------------------------

    fun testOperatorDeclarationsAreNamedElements() {
        val f = file()
        val op = named(f).firstOrNull { it.name == "operator+" }
        assertNotNull("operator+ is not a named element; have: " +
            named(f).map { it.name }, op)
        assertEquals("+", op!!.nameIdentifier!!.text)
    }

    // ---- 5.1.6 — structure view regression guard --------------------------------

    fun testStructureViewListsTheMembers() {
        val f = file()
        val builder = CajetaStructureViewFactory().getStructureViewBuilder(f)
            as com.intellij.ide.structureView.TreeBasedStructureViewBuilder
        val model = builder.createStructureViewModel(null)
        try {
            val labels = mutableListOf<String>()
            fun walk(e: com.intellij.ide.structureView.StructureViewTreeElement) {
                labels += e.presentation.presentableText ?: "?"
                e.children.forEach {
                    walk(it as com.intellij.ide.structureView.StructureViewTreeElement)
                }
            }
            walk(model.root)

            // Everything the old view showed (top-level types via its
            // direct-child filter) ...
            for (expected in listOf("Outer", "Greeter", "Color", "Retry",
                                    "package demo.app")) {
                assertTrue("structure view lost '$expected': $labels",
                    labels.contains(expected))
            }
            // ... plus the members the old identifier-BFS never reached
            // (its direct-children filter stopped at classBody).
            for (expected in listOf("bump", "count", "backup", "greet",
                                    "RED", "GREEN", "operator+", "Inner")) {
                assertTrue("structure view misses member '$expected': $labels",
                    labels.contains(expected))
            }
            // Locals, parameters, and type parameters are NOT structure.
            for (unexpected in listOf("next", "delta", "seed", "who", "T")) {
                assertFalse("structure view must not list '$unexpected': $labels",
                    labels.contains(unexpected))
            }
        } finally {
            model.dispose()
        }
    }
}
