package dev.cajeta.idea.xref

import com.intellij.psi.PsiElement
import com.intellij.psi.PsiFile
import com.intellij.psi.PsiPolyVariantReference
import com.intellij.testFramework.fixtures.BasePlatformTestCase
import dev.cajeta.idea.psi.CajetaNamedElement
import dev.cajeta.idea.psi.CajetaParameterDeclaration
import dev.cajeta.idea.psi.CajetaVariableName

/**
 * ide-symbol-index Unit 7 (plan 7.1.1 - 7.1.6): references are ADAPTERS. Each
 * looks its own (file, line, col) up in the index and returns the PSI element
 * at the compiler's answer — no scope walking, no import resolution, no
 * overload selection in Kotlin (§4.0.2). The ONE exception is the local
 * fallback (§4.3): locals, parameters, and type parameters bind within the
 * subtree they are written in, cannot be got wrong, and must keep resolving in
 * a buffer the compiler has not re-seen.
 */
class CajetaXrefReferenceTest : BasePlatformTestCase() {

    // ---- helpers ---------------------------------------------------------------

    private fun line(rel: String, record: String) =
        """{"kind":"xref","rel":"$rel","record":$record}"""

    private val version = line("version", """{"major": 1, "minor": 0}""")

    private fun addShard(name: String, vararg lines: String) =
        myFixture.addFileToProject("${CajetaXrefShards.DIR}/$name.cjxref",
            (listOf(version) + lines).joinToString("\n"))

    /** 1-based line and 0-based column of the FIRST occurrence of [needle]. */
    private fun lineColOf(text: String, needle: String): Pair<Int, Int> {
        val at = text.indexOf(needle)
        check(at >= 0) { "needle '$needle' not in text" }
        val before = text.substring(0, at)
        val ln = before.count { it == '\n' } + 1
        val col = at - (before.lastIndexOf('\n') + 1)
        return ln to col
    }

    private fun refAt(file: PsiFile, needle: String, occurrence: Int = 1): PsiPolyVariantReference? {
        var at = -1
        repeat(occurrence) { at = file.text.indexOf(needle, at + 1) }
        check(at >= 0)
        val leaf = file.findElementAt(at)!!
        var e: PsiElement? = leaf
        while (e != null && e.reference == null) e = e.parent
        return e?.reference as? PsiPolyVariantReference
    }

    private fun enclosingNamed(e: PsiElement?): CajetaNamedElement? =
        generateSequence(e) { it.parent }
            .filterIsInstance<CajetaNamedElement>().firstOrNull()

    // ---- 7.1.1 — type reference via the index, across files ----------------------

    fun testTypeReferenceResolvesAcrossFilesViaTheIndex() {
        val helperText = "package demo;\npublic class Helper {\n}\n"
        myFixture.addFileToProject("demo/Helper.cajeta", helperText)
        val targetText =
            "package demo;\npublic class Target {\n    Helper aide;\n}\n"
        val target = myFixture.addFileToProject("demo/Target.cajeta", targetText)

        val (dl, dc) = lineColOf(helperText, "Helper")
        val (ul, uc) = lineColOf(targetText, "Helper")
        addShard("demo_Helper",
            line("declarations",
                """{"fqn": "demo.Helper", "kind": "class", "file": "demo/Helper.cajeta", "line": $dl, "col": $dc}"""))
        addShard("demo_Target",
            line("references",
                """{"target": "demo.Helper", "kind": "type", "file": "demo/Target.cajeta", "line": $ul, "col": $uc}"""))

        val resolved = refAt(target, "Helper")!!.resolve()
        assertNotNull("type ref did not resolve", resolved)
        val named = enclosingNamed(resolved)!!
        assertEquals("Helper", named.name)
        assertEquals("Helper.cajeta", named.containingFile.name)
    }

    // ---- 7.1.2 — the compiler-chosen overload -----------------------------------

    fun testCallResolvesToTheCompilerChosenOverload() {
        val svcText = "package demo;\npublic class Svc {\n" +
            "    public void run(int32 n) {\n    }\n" +
            "    public void run(Svc other) {\n    }\n}\n"
        myFixture.addFileToProject("demo/Svc.cajeta", svcText)
        val userText = "package demo;\npublic class User {\n" +
            "    public void go(Svc s) {\n        s.run(1);\n    }\n}\n"
        val user = myFixture.addFileToProject("demo/User.cajeta", userText)

        // Two overloads at different lines; the compiler chose the int32 one.
        val intKey = "demo.Svc::run(pointer,int32)"
        val svcKey = "demo.Svc::run(pointer,demo.Svc)"
        val (l1, c1) = lineColOf(svcText, "run(int32")
        val (l2, c2) = lineColOf(svcText, "run(Svc")
        val (ul, uc) = lineColOf(userText, "run(1")
        addShard("demo_Svc",
            line("declarations",
                """{"fqn": "demo.Svc.run", "kind": "method", "owner": "demo.Svc", "overloadKey": "$intKey", "file": "demo/Svc.cajeta", "line": $l1, "col": $c1}"""),
            line("declarations",
                """{"fqn": "demo.Svc.run", "kind": "method", "owner": "demo.Svc", "overloadKey": "$svcKey", "file": "demo/Svc.cajeta", "line": $l2, "col": $c2}"""))
        addShard("demo_User",
            line("calls",
                """{"callee": "$intKey", "caller": "demo.User::go(pointer,demo.Svc)", "file": "demo/User.cajeta", "line": $ul, "col": $uc}"""))

        val resolved = refAt(user, "run(1")!!.resolve()
        assertNotNull("call did not resolve", resolved)
        val doc = myFixture.getDocument(resolved!!.containingFile)!!
        assertEquals("resolved to the wrong overload",
            l1, doc.getLineNumber(resolved.textOffset) + 1)
    }

    // ---- 7.1.3 — ambiguity is a candidate set, not a guess -----------------------

    fun testAmbiguityYieldsTheCandidateSetNotAGuess() {
        val aText = "package demo;\npublic class Dup {\n}\n"
        val bText = "package other;\npublic class Dup {\n}\n"
        myFixture.addFileToProject("demo/Dup.cajeta", aText)
        myFixture.addFileToProject("other/Dup.cajeta", bText)
        val useText = "package demo;\npublic class UseIt {\n    Dup d;\n}\n"
        val use = myFixture.addFileToProject("demo/UseIt.cajeta", useText)

        // The compiler exported one target FQN but TWO declarations claim it
        // (e.g. mid-refactor duplicate). The reference must present both.
        val (l1, c1) = lineColOf(aText, "Dup")
        val (l2, c2) = lineColOf(bText, "Dup")
        val (ul, uc) = lineColOf(useText, "Dup d")
        addShard("demo_Dup",
            line("declarations",
                """{"fqn": "demo.Dup", "kind": "class", "file": "demo/Dup.cajeta", "line": $l1, "col": $c1}"""))
        addShard("other_Dup",
            line("declarations",
                """{"fqn": "demo.Dup", "kind": "class", "file": "other/Dup.cajeta", "line": $l2, "col": $c2}"""))
        addShard("demo_UseIt",
            line("references",
                """{"target": "demo.Dup", "kind": "type", "file": "demo/UseIt.cajeta", "line": $ul, "col": $uc}"""))

        val ref = refAt(use, "Dup d")!!
        assertEquals(2, ref.multiResolve(false).size)
        assertNull("ambiguity must not be guessed away", ref.resolve())
    }

    // ---- 7.1.4 — no entry, no throw ----------------------------------------------

    fun testNoIndexEntryResolvesToNullWithoutThrowing() {
        val text = "package demo;\npublic class Lone {\n    Ghost g;\n}\n"
        val file = myFixture.addFileToProject("demo/Lone.cajeta", text)
        val ref = refAt(file, "Ghost")
        assertNull(ref?.resolve())
    }

    // ---- 7.1.5 — the local fallback ------------------------------------------------

    fun testLocalsParamsAndTypeParamsResolveWithTheIndexEmpty() {
        val text = "package demo;\npublic class Calc<T> {\n" +
            "    public int32 bump(int32 delta) {\n" +
            "        int32 next = delta;\n" +
            "        return next;\n" +
            "    }\n" +
            "    public T pick(T seed) {\n        return seed;\n    }\n}\n"
        val file = myFixture.addFileToProject("demo/Calc.cajeta", text)

        // Local read two lines below its declaration.
        val local = refAt(file, "next", occurrence = 2)!!.resolve()
        assertTrue("local read must resolve to its declarator",
            local is CajetaVariableName && (local as CajetaVariableName).name == "next")

        // Parameter read.
        val param = refAt(file, "delta", occurrence = 2)!!.resolve()
        assertTrue("parameter read must resolve to the parameter",
            enclosingNamed(param) is CajetaParameterDeclaration)

        // Type parameter in return-type position.
        val tp = refAt(file, "T pick")!!.resolve()
        assertNotNull("type parameter must resolve from PSI alone", tp)
        assertEquals("T", enclosingNamed(tp)!!.name)
    }

    // ---- 7.1.6 — the fallback is confined ------------------------------------------

    fun testAFieldAccessDoesNotResolveFromPsiAlone() {
        val text = "package demo;\npublic class Holder {\n" +
            "    int32 count;\n" +
            "    public int32 read() {\n        return count;\n    }\n}\n"
        val file = myFixture.addFileToProject("demo/Holder.cajeta", text)

        // `count` binds through the class scope — the compiler's business.
        // With no index entry it stays UNRESOLVED rather than guessed (§4.3:
        // the test is not "is it small" but "can the IDE be wrong about it").
        val ref = refAt(file, "count", occurrence = 2)
        assertNull(ref?.resolve())
    }
}
