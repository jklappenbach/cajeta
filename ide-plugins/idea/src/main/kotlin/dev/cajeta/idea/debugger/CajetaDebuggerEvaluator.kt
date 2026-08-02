package dev.cajeta.idea.debugger

import com.intellij.openapi.editor.Document
import com.intellij.openapi.project.Project
import com.intellij.openapi.util.TextRange
import com.intellij.xdebugger.XSourcePosition
import com.intellij.xdebugger.evaluation.EvaluationMode
import com.intellij.xdebugger.evaluation.ExpressionInfo
import com.intellij.xdebugger.evaluation.XDebuggerEvaluator

/**
 * Editor hover evaluation (debugger-variable-inspection spec §7.1.3).
 *
 * A thin adapter: [CajetaHoverExpression] decides what to ask about,
 * [CajetaDebugSession.evaluate] asks, and [CajetaValue] renders the answer —
 * the same class the Variables view uses, so a hovered aggregate expands with
 * identical rows rather than a second, subtly different presentation.
 *
 * Bound to a FRAME, because that is the scope an identifier resolves in: the
 * frame the reader has selected is the one the popup answers for.
 */
class CajetaDebuggerEvaluator(
    private val session: CajetaDebugSession,
    private val frameId: Int,
) : XDebuggerEvaluator() {

    override fun evaluate(
        expression: String,
        callback: XEvaluationCallback,
        expressionPosition: XSourcePosition?,
    ) {
        // Reject here what the server would reject anyway, so a hover over
        // `f()` says so immediately instead of paying a round trip (§7.2.3).
        if (!CajetaHoverExpression.isSimplePath(expression)) {
            callback.errorOccurred("unsupported expression: $expression")
            return
        }
        session.evaluate(expression, frameId)
            .thenAccept { outcome ->
                val value = outcome.value
                if (value != null) callback.evaluated(CajetaValue(value, session))
                // §7.2.4: an identifier that is not live is a plain message,
                // never an error dialog.
                else callback.errorOccurred(outcome.message ?: "not available")
            }
            .exceptionally { t ->
                callback.errorOccurred(t.message ?: "evaluation failed")
                null
            }
    }

    /**
     * What the hover asks about. The platform default takes the bare word at
     * the offset, which turns `origin.x` into `x` — an identifier the frame
     * does not have. Widening to the simple-path grammar makes the hover ask
     * the question the server can actually answer (§7.1.2, §7.2.2).
     */
    override fun getExpressionInfoAtOffset(
        project: Project,
        document: Document,
        offset: Int,
        sideEffectsAllowed: Boolean,
    ): ExpressionInfo? {
        val span = CajetaHoverExpression.at(document.charsSequence, offset) ?: return null
        return ExpressionInfo(TextRange(span.start, span.end), span.expression)
    }

    /** Evaluation never mutates the program (§7.1.4), so the inline/expression
     *  distinction is presentational only. */
    override fun isCodeFragmentEvaluationSupported(): Boolean = false

    override fun getEvaluationMode(
        text: String,
        startOffset: Int,
        endOffset: Int,
        psiFile: com.intellij.psi.PsiFile?,
    ): EvaluationMode = EvaluationMode.EXPRESSION
}
