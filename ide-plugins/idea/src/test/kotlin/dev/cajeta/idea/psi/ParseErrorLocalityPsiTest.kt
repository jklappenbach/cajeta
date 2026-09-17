package dev.cajeta.idea.psi

import com.intellij.psi.PsiElement
import com.intellij.psi.PsiFile
import com.intellij.psi.PsiPolyVariantReference
import com.intellij.psi.util.PsiTreeUtil
import com.intellij.testFramework.fixtures.BasePlatformTestCase
import dev.cajeta.idea.xref.CajetaXrefShards

/**
 * parse-error-locality 2.1.3 – 2.1.5 (spec §4.2.1, §4.2.5): with one method
 * broken, the rest of the file is still PSI — named, packaged, and resolvable
 * against the retained shard.
 */
class ParseErrorLocalityPsiTest : BasePlatformTestCase() {

    private fun line(rel: String, record: String) =
        """{"kind":"xref","rel":"$rel","record":$record}"""

    private val version = line("version", """{"major": 1, "minor": 0}""")

    private fun addShard(name: String, vararg lines: String) =
        myFixture.addFileToProject("${CajetaXrefShards.DIR}/$name.cjxref",
            (listOf(version) + lines).joinToString("\n"))

    private fun lineColOf(text: String, needle: String): Pair<Int, Int> {
        val at = text.indexOf(needle)
        check(at >= 0) { "needle '$needle' not in text" }
        val before = text.substring(0, at)
        return (before.count { it == '\n' } + 1) to (at - (before.lastIndexOf('\n') + 1))
    }

    private fun refAt(file: PsiFile, needle: String): PsiPolyVariantReference? {
        val at = file.text.indexOf(needle)
        check(at >= 0)
        var e: PsiElement? = file.findElementAt(at)
        while (e != null && e.reference == null) e = e.parent
        return e?.reference as? PsiPolyVariantReference
    }

    private fun unit(name: String, second: String) = """
        package demo;

        public final class $name {
            int32 total;

            public int32 first() {
                int32 a = 1;
                return a;
            }

            public int32 second() {
                $second
                return b;
            }
        }
    """.trimIndent() + "\n"

    private val brokenUnits = mapOf(
        "Broken" to unit("Broken", "int32 b = 2"),
        "Paren" to unit("Paren", "int32 b = (2;"),
        "Quote" to unit("Quote", "String s = \"abc;"),
        "Brace" to """
            package demo;

            public final class Brace {
                int32 total;

                public int32 first() {
                    int32 a = 1;
                    return a;

                public int32 second() {
                    int32 b = 2;
                    return b;
                }
            }
        """.trimIndent() + "\n",
        "Two" to """
            package demo;

            public final class Alpha {
                public int32 run() {
                    int32 x = 1
                    return x;
                }
            }

            public final class Beta {
                public int32 go() {
                    int32 y = 2;
                    return y;
                }
            }
        """.trimIndent() + "\n",
    )

    // ---- 2.1.3 / spec 4.2.1 — a use in the intact method still resolves --------

    fun testAUseOutsideTheBrokenMethodResolvesViaTheIndex() {
        val helperText = "package demo;\npublic class Helper {\n}\n"
        myFixture.addFileToProject("demo/Helper.cajeta", helperText)
        val targetText = "package demo;\npublic class Target {\n" +
            "    public void first() {\n        Helper h = null;\n    }\n" +
            "    public int32 second() {\n        int32 b = 2\n        return b;\n    }\n}\n"
        val target = myFixture.addFileToProject("demo/Target.cajeta", targetText)

        val (dl, dc) = lineColOf(helperText, "Helper")
        val (ul, uc) = lineColOf(targetText, "Helper h")
        addShard("demo_Helper",
            line("declarations",
                """{"fqn": "demo.Helper", "kind": "class", "file": "demo/Helper.cajeta", "line": $dl, "col": $dc}"""))
        addShard("demo_Target",
            line("references",
                """{"target": "demo.Helper", "kind": "type", "file": "demo/Target.cajeta", "line": $ul, "col": $uc}"""))

        val ref = refAt(target, "Helper h")
        assertNotNull("no reference on the use in the intact method", ref)
        val resolved = ref!!.resolve()
        assertNotNull("use outside the broken method did not resolve", resolved)
        assertEquals("Helper.cajeta", resolved!!.containingFile.name)
    }

    // ---- 2.1.4 — the package survives every break ---------------------------------

    fun testThePackageIsFoundInEveryBrokenBuffer() {
        for ((name, text) in brokenUnits) {
            val file = myFixture.configureByText("$name.cajeta", text)
            assertEquals(name, "demo", packageOf(file))
        }
    }

    // ---- 2.1.5 / spec 4.2.5 — declarations outside the break are named elements --

    fun testDeclarationsOutsideTheBreakAreStillNamed() {
        val expected = mapOf(
            "Broken" to listOf("Broken", "total", "first", "a", "second", "b"),
            "Paren" to listOf("Paren", "total", "first", "a", "second", "b"),
            "Quote" to listOf("Quote", "total", "first", "a", "second"),
            "Brace" to listOf("Brace", "total", "first", "a", "b"),
            "Two" to listOf("Alpha", "run", "x", "Beta", "go", "y"),
        )
        for ((name, text) in brokenUnits) {
            val file = myFixture.configureByText("$name.cajeta", text)
            val names = PsiTreeUtil.findChildrenOfType(file, CajetaNamedElement::class.java)
                .mapNotNull { it.name }
            for (n in expected.getValue(name)) {
                assertTrue("$name: '$n' is not a named element in $names", n in names)
            }
        }
    }
}
