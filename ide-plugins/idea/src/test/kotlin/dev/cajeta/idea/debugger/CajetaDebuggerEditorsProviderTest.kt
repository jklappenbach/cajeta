package dev.cajeta.idea.debugger

import com.intellij.testFramework.fixtures.BasePlatformTestCase
import com.intellij.xdebugger.evaluation.EvaluationMode
import com.intellij.xdebugger.impl.breakpoints.XExpressionImpl

/**
 * The Watch/Evaluate expression editor asks [CajetaDebuggerEditorsProvider]
 * for a Document backing the typed fragment. The fragment PSI file is
 * non-physical, and `PsiDocumentManager.getDocument()` returns null for
 * non-eventful files on newer platforms (gated on supportsSendingPsiEvents) —
 * live symptom: IllegalStateException "could not create a document for the
 * debugger fragment" the moment the debugger UI builds a watch row. The
 * provider must therefore read the view provider's document, which exists
 * regardless of the event system.
 */
class CajetaDebuggerEditorsProviderTest : BasePlatformTestCase() {

    private fun createDocText(expr: String): String {
        val provider = CajetaDebuggerEditorsProvider()
        val doc = provider.createDocument(
            project,
            XExpressionImpl.fromText(expr)!!,
            null,
            EvaluationMode.EXPRESSION,
        )
        return doc.text
    }

    fun testFragmentDocumentCarriesTheExpressionText() {
        assertEquals("a + b", createDocText("a + b"))
    }

    fun testEmptyExpressionStillYieldsADocument() {
        // The combo box creates a document for the empty prompt before
        // anything is typed — the exact call that blew up live.
        assertEquals("", createDocText(""))
    }
}
