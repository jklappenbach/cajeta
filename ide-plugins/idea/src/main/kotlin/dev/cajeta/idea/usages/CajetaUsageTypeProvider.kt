package dev.cajeta.idea.usages

import com.intellij.psi.PsiElement
import com.intellij.usages.impl.rules.UsageType
import com.intellij.usages.impl.rules.UsageTypeProvider
import dev.cajeta.idea.debugger.Json
import dev.cajeta.idea.psi.CajetaIdentifier
import dev.cajeta.idea.xref.CajetaXrefReference
import dev.cajeta.idea.xref.XrefQuery

/**
 * Groups the usage tree by what the reference DOES (ide-features §2.1.2):
 * read / write / call / inherit / import, the way Java's usage view separates
 * them, instead of one undifferentiated list.
 *
 * The classification is the compiler's, carried on each exported record —
 * this only maps it onto the platform's vocabulary.
 */
class CajetaUsageTypeProvider : UsageTypeProvider {

    override fun getUsageType(element: PsiElement): UsageType? {
        val id = element as? CajetaIdentifier ?: return null
        val file = id.containingFile ?: return null
        val rel = CajetaXrefReference.xrefRelPath(file) ?: return null
        val doc = com.intellij.psi.PsiDocumentManager.getInstance(id.project)
            .getDocument(file) ?: return null
        val offset = id.textOffset
        if (offset >= doc.textLength) return null
        val line = doc.getLineNumber(offset) + 1
        val col = offset - doc.getLineStartOffset(line - 1)

        val here = XrefQuery.usesIn(id.project, rel)
            .mapNotNull { CajetaUsageRecords.parse(it) }
            .firstOrNull { it.line == line && it.col == col }
            ?: return null
        return when (here.kind) {
            CajetaUsageKind.READ -> UsageType.READ
            CajetaUsageKind.WRITE -> UsageType.WRITE
            CajetaUsageKind.CALL -> UsageType.UNCLASSIFIED
            CajetaUsageKind.INHERIT -> UsageType.CLASS_EXTENDS_IMPLEMENTS_LIST
            CajetaUsageKind.IMPORT -> UsageType.CLASS_IMPORT
            CajetaUsageKind.TYPE -> UsageType.CLASS_INSTANCE_OF
            CajetaUsageKind.OTHER -> null
        }
    }
}
