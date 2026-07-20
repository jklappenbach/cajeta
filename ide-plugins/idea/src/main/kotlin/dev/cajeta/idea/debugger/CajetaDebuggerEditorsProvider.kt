package dev.cajeta.idea.debugger

import com.intellij.openapi.editor.Document
import com.intellij.openapi.fileTypes.FileType
import com.intellij.openapi.project.Project
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
        // NOT PsiDocumentManager.getDocument(): the fragment file is
        // non-physical (no event system), and since 2026.x getDocument returns
        // null for those (PsiDocumentManagerBase gates on
        // supportsSendingPsiEvents), which broke the Watch/Evaluate editor
        // with "could not create a document". fileDocument reads the view
        // provider's document directly, which has no such gate on any
        // platform version.
        return psiFile.fileDocument
    }
}
