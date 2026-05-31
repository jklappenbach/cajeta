package dev.cajeta.idea.debugger

import com.intellij.openapi.editor.Document
import com.intellij.openapi.fileTypes.FileType
import com.intellij.openapi.project.Project
import com.intellij.psi.PsiDocumentManager
import com.intellij.psi.PsiFileFactory
import com.intellij.xdebugger.XExpression
import com.intellij.xdebugger.XSourcePosition
import com.intellij.xdebugger.evaluation.EvaluationMode
import com.intellij.xdebugger.evaluation.XDebuggerEditorsProvider
import dev.cajeta.idea.CajetaFileType

/**
 * Provides Cajeta-highlighted editors for breakpoint conditions and the
 * Watch/Evaluate window. Wired now so the XDebugProcess skeleton is complete;
 * expression evaluation itself lands in CP6e.
 */
class CajetaDebuggerEditorsProvider : XDebuggerEditorsProvider() {

    override fun getFileType(): FileType = CajetaFileType

    override fun createDocument(
        project: Project,
        expression: XExpression,
        sourcePosition: XSourcePosition?,
        mode: EvaluationMode,
    ): Document {
        val psiFile = PsiFileFactory.getInstance(project)
            .createFileFromText("cajetaFragment.cajeta", CajetaFileType, expression.expression)
        return PsiDocumentManager.getInstance(project).getDocument(psiFile)
            ?: throw IllegalStateException("could not create a document for the debugger fragment")
    }
}
