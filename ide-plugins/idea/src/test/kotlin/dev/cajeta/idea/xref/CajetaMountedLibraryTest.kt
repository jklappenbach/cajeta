package dev.cajeta.idea.xref

import com.intellij.openapi.vfs.LocalFileSystem
import com.intellij.openapi.vfs.VfsUtil
import com.intellij.psi.PsiElement
import com.intellij.psi.PsiPolyVariantReference
import com.intellij.testFramework.PsiTestUtil
import com.intellij.testFramework.fixtures.BasePlatformTestCase
import dev.cajeta.idea.psi.CajetaNamedElement
import java.nio.file.Files

/**
 * ide-symbol-index Unit 8 (plan 8.1.5): mounted sources are indexed — a
 * mounted type resolves and its subtypes are findable. The mount is a plain
 * read-only directory attached as a library SOURCE root; resolution rides
 * the same shard + FilenameIndex path as project files (Unit 7's locate()
 * already searches allScope).
 */
class CajetaMountedLibraryTest : BasePlatformTestCase() {

    private fun line(rel: String, record: String) =
        """{"kind":"xref","rel":"$rel","record":$record}"""

    private val version = line("version", """{"major": 1, "minor": 0}""")

    fun testAMountedTypeResolvesAndItsSubtypesAreFindable() {
        // The "mounted stdlib": a local-fs directory, library source root.
        val mountDir = Files.createTempDirectory("cajeta-mounted-lib")
        val objFile = mountDir.resolve("cajeta/lang/Object.cajeta")
        Files.createDirectories(objFile.parent)
        val objText = "package cajeta.lang;\npublic class Object {\n}\n"
        Files.write(objFile, objText.toByteArray())
        val vDir = LocalFileSystem.getInstance()
            .refreshAndFindFileByNioFile(mountDir)!!
        VfsUtil.markDirtyAndRefresh(false, true, true, vDir)
        PsiTestUtil.addProjectLibrary(
            module, "cajeta-stdlib-src", emptyList(), listOf(vDir))

        // Project file referencing the mounted type, with shards for both.
        val useText = "package demo;\npublic class Use {\n    Object o;\n}\n"
        val use = myFixture.addFileToProject("demo/Use.cajeta", useText)
        val dl = 2; val dc = objText.split('\n')[1].indexOf("Object")
        val ul = 3; val uc = useText.split('\n')[2].indexOf("Object")
        myFixture.addFileToProject("${CajetaXrefShards.DIR}/lang_Object.cjxref",
            listOf(version,
                line("declarations",
                    """{"fqn": "cajeta.lang.Object", "kind": "class", "file": "cajeta/lang/Object.cajeta", "line": $dl, "col": $dc}""")
            ).joinToString("\n"))
        myFixture.addFileToProject("${CajetaXrefShards.DIR}/demo_Use.cjxref",
            listOf(version,
                line("references",
                    """{"target": "cajeta.lang.Object", "kind": "type", "file": "demo/Use.cajeta", "line": $ul, "col": $uc}"""),
                line("inheritance",
                    """{"child": "demo.Use", "parent": "cajeta.lang.Object", "kind": "extends", "file": "demo/Use.cajeta", "line": 2, "col": 13}""")
            ).joinToString("\n"))

        // The mounted type resolves...
        val at = use.text.indexOf("Object o")
        var e: PsiElement? = use.findElementAt(at)
        while (e != null && e.reference == null) e = e.parent
        val resolved = (e?.reference as? PsiPolyVariantReference)?.resolve()
        assertNotNull("mounted type did not resolve", resolved)
        val named = generateSequence(resolved) { it.parent }
            .filterIsInstance<CajetaNamedElement>().first()
        assertEquals("Object", named.name)
        assertTrue("resolved outside the mount",
            named.containingFile.virtualFile.path.startsWith(
                vDir.path))

        // ...and its subtypes are findable.
        assertEquals(listOf("demo.Use"),
            XrefQuery.subtypesOf(project, "cajeta.lang.Object")
                .map { it.at("child").asString() })
    }
}
