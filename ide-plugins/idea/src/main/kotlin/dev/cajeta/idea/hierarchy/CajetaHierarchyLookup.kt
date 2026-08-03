package dev.cajeta.idea.hierarchy

import com.intellij.openapi.project.Project
import com.intellij.psi.PsiDocumentManager
import com.intellij.psi.PsiElement
import com.intellij.psi.PsiManager
import com.intellij.psi.search.FilenameIndex
import com.intellij.psi.search.GlobalSearchScope
import com.intellij.psi.util.PsiTreeUtil
import dev.cajeta.idea.debugger.Json
import dev.cajeta.idea.psi.CajetaTypeDeclaration
import dev.cajeta.idea.xref.XrefQuery

/**
 * An FQN back to the declaration it names, so a hierarchy node can be opened.
 *
 * The index answers where a type is declared; this only follows that answer to
 * the PSI, the same way the reference adapter does. A type the index knows but
 * whose file has moved on yields null — a node that cannot be navigated is
 * better dropped than shown pointing at the wrong line (3.1.5).
 */
object CajetaHierarchyLookup {

    fun declarationOf(project: Project, fqn: String): PsiElement? {
        for (record in XrefQuery.declarationsOf(project, fqn)) {
            elementFor(project, record)?.let { return it }
        }
        return null
    }

    private fun elementFor(project: Project, record: Json.Obj): PsiElement? {
        val rel = (record.entries["file"] as? Json.Str)?.value?.ifBlank { null } ?: return null
        val line = (record.entries["line"] as? Json.Num)?.value?.toInt() ?: return null
        if (line <= 0) return null
        val col = (record.entries["col"] as? Json.Num)?.value?.toInt() ?: 0
        val base = rel.substringAfterLast('/')
        if (base.isBlank()) return null
        val scope = GlobalSearchScope.allScope(project)
        // One relative path can match several real files — a stdlib mount per
        // compiler build, an archive extracted twice — so take the first that
        // actually yields a type declaration.
        for (vFile in FilenameIndex.getVirtualFilesByName(base, scope)) {
            if (!vFile.path.endsWith("/$rel") && vFile.path != rel) continue
            val psi = PsiManager.getInstance(project).findFile(vFile) ?: continue
            val doc = PsiDocumentManager.getInstance(project).getDocument(psi) ?: continue
            if (line > doc.lineCount) continue
            val offset = doc.getLineStartOffset(line - 1) + col
            if (offset >= doc.textLength) continue
            val at = psi.findElementAt(offset) ?: continue
            PsiTreeUtil.getParentOfType(at, CajetaTypeDeclaration::class.java, false)
                ?.let { return it }
        }
        return null
    }
}
