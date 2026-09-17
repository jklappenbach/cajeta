package dev.cajeta.idea.xref

import com.intellij.openapi.command.WriteCommandAction
import com.intellij.openapi.editor.Document
import com.intellij.openapi.editor.EditorFactory
import com.intellij.openapi.editor.event.EditorFactoryEvent
import com.intellij.psi.PsiDocumentManager
import com.intellij.psi.PsiElement
import com.intellij.psi.PsiFile
import com.intellij.psi.PsiPolyVariantReference
import com.intellij.testFramework.fixtures.BasePlatformTestCase
import dev.cajeta.idea.debugger.Json
import dev.cajeta.idea.lint.XrefRecord

/**
 * parse-error-locality 3.1.1 – 3.1.4 (spec §4.2.2 – §4.2.4): a use whose line
 * moved while the buffer is broken still resolves against the retained shard,
 * a moved use whose name changed resolves to nothing, and pins are dropped
 * when a new shard or a closed editor supersedes them.
 */
class CajetaXrefPositionsTest : BasePlatformTestCase() {

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
        check(at >= 0) { "needle '$needle' not in text" }
        var e: PsiElement? = file.findElementAt(at)
        while (e != null && e.reference == null) e = e.parent
        return e?.reference as? PsiPolyVariantReference
    }

    private fun useRecord(line: Int, col: Int): String =
        """{"target": "demo.Helper", "kind": "type", "file": "demo/Target.cajeta", "line": $line, "col": $col}"""

    private fun record(line: Int, col: Int) =
        XrefRecord("references", Json.parse(useRecord(line, col)) as Json.Obj)

    private val targetText = "package demo;\npublic class Target {\n" +
        "    int32 total;\n" +
        "    public void first() {\n        Helper h = null;\n    }\n" +
        "    public int32 second() {\n        int32 b = 2;\n        return b;\n    }\n}\n"

    private class Scene(val file: PsiFile, val doc: Document, val useLine: Int, val useCol: Int)

    /** Helper + Target on disk, the shard recording the use in `first()`, pins set. */
    private fun scene(): Scene {
        val helperText = "package demo;\npublic class Helper {\n}\n"
        myFixture.addFileToProject("demo/Helper.cajeta", helperText)
        val target = myFixture.addFileToProject("demo/Target.cajeta", targetText)
        val (dl, dc) = lineColOf(helperText, "Helper")
        val (ul, uc) = lineColOf(targetText, "Helper h")
        addShard("demo_Helper",
            line("declarations",
                """{"fqn": "demo.Helper", "kind": "class", "file": "demo/Helper.cajeta", "line": $dl, "col": $dc}"""))
        addShard("demo_Target", line("references", useRecord(ul, uc)))
        val doc = PsiDocumentManager.getInstance(project).getDocument(target)!!
        CajetaXrefPositions.anchor(doc, listOf(record(ul, uc)))
        return Scene(target, doc, ul, uc)
    }

    private fun edit(doc: Document, action: (Document) -> Unit) {
        WriteCommandAction.runWriteCommandAction(project) { action(doc) }
        PsiDocumentManager.getInstance(project).commitAllDocuments()
    }

    private fun resolvedFileOf(file: PsiFile, needle: String): String? =
        refAt(file, needle)?.resolve()?.containingFile?.name

    // ---- 3.1.1 — the use moved down, the buffer is broken, it still resolves ----

    fun testAUseBelowAnInsertedBrokenLineStillResolves() {
        val s = scene()
        edit(s.doc) { it.insertString(it.getLineStartOffset(2), "    int32 extra\n") }
        assertEquals("Helper.cajeta", resolvedFileOf(s.file, "Helper h"))
    }

    fun testAUseBelowADeletedLineStillResolves() {
        val s = scene()
        edit(s.doc) { it.deleteString(it.getLineStartOffset(2), it.getLineStartOffset(3)) }
        assertEquals("Helper.cajeta", resolvedFileOf(s.file, "Helper h"))
    }

    fun testWithoutPinsAMovedUseDoesNotResolve() {
        val s = scene()
        CajetaXrefPositions.clear(s.doc)
        edit(s.doc) { it.insertString(it.getLineStartOffset(2), "    int32 extra\n") }
        assertNull(resolvedFileOf(s.file, "Helper h"))
    }

    // ---- 3.1.2 — a new shard supersedes the pins -----------------------------------

    fun testANewShardSupersedesThePins() {
        val s = scene()
        edit(s.doc) { it.insertString(it.getLineStartOffset(2), "    int32 extra;\n") }
        val moved = CajetaXrefPositions.Pos(s.useLine + 1, s.useCol)
        addShard("demo_Target_relint", line("references", useRecord(moved.line, moved.col)))
        CajetaXrefPositions.anchor(s.doc, listOf(record(moved.line, moved.col)))

        val useOffset = s.doc.getLineStartOffset(moved.line - 1) + moved.col
        assertEquals(moved, CajetaXrefPositions.recordedPositionAt(s.doc, useOffset))
        val oldOffset = s.doc.getLineStartOffset(s.useLine - 1) + s.useCol
        assertNull(CajetaXrefPositions.recordedPositionAt(s.doc, oldOffset))
        assertEquals("Helper.cajeta", resolvedFileOf(s.file, "Helper h"))
    }

    // ---- 3.1.3 — wrong > missing: a moved use whose name changed ------------------

    fun testAMovedUseWhoseNameChangedResolvesToNothing() {
        val s = scene()
        edit(s.doc) {
            val at = it.text.indexOf("Helper h")
            it.replaceString(at, at + "Helper".length, "Other")
            it.insertString(it.getLineStartOffset(2), "    int32 extra\n")
        }
        assertNull(resolvedFileOf(s.file, "Other h"))
    }

    // ---- 3.1.4 — pins are released --------------------------------------------------

    fun testClearDropsThePins() {
        val s = scene()
        val useOffset = s.doc.getLineStartOffset(s.useLine - 1) + s.useCol
        assertNotNull(CajetaXrefPositions.recordedPositionAt(s.doc, useOffset))
        CajetaXrefPositions.clear(s.doc)
        assertNull(CajetaXrefPositions.recordedPositionAt(s.doc, useOffset))
    }

    fun testClosingTheLastEditorDropsThePins() {
        val s = scene()
        myFixture.openFileInEditor(s.file.virtualFile)
        val useOffset = s.doc.getLineStartOffset(s.useLine - 1) + s.useCol
        assertNotNull(CajetaXrefPositions.recordedPositionAt(s.doc, useOffset))
        CajetaXrefPositions.EditorClosed()
            .editorReleased(EditorFactoryEvent(EditorFactory.getInstance(), myFixture.editor))
        assertNull(CajetaXrefPositions.recordedPositionAt(s.doc, useOffset))
    }
}
