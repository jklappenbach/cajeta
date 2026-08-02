package dev.cajeta.idea.debugger

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.application.ModalityState
import com.intellij.openapi.editor.Document
import com.intellij.openapi.project.Project
import com.intellij.openapi.util.TextRange
import com.intellij.xdebugger.XSourcePosition
import com.intellij.xdebugger.evaluation.EvaluationMode
import com.intellij.xdebugger.evaluation.ExpressionInfo
import com.intellij.xdebugger.evaluation.XDebuggerEvaluator
import java.util.concurrent.TimeUnit

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
            // A hover is a glance, not a query: if the answer is not back
            // within HOVER_TIMEOUT the reader has moved on, and delivering it
            // then would pop a hint over whatever they are now looking at.
            // Ending in a terminal state matters — a callback that never fires
            // leaves the platform showing "Evaluating..." indefinitely.
            .orTimeout(HOVER_TIMEOUT_SECONDS, TimeUnit.SECONDS)
            .thenAccept { outcome ->
                val value = outcome.value
                onEdt {
                    if (value != null) callback.evaluated(CajetaValue(value, session))
                    // §7.2.4: an identifier that is not live is a plain message,
                    // never an error dialog.
                    else callback.errorOccurred(outcome.message ?: "not available")
                }
            }
            .exceptionally { t ->
                val why = if (t is java.util.concurrent.TimeoutException ||
                              t.cause is java.util.concurrent.TimeoutException)
                    "evaluation timed out" else (t.message ?: "evaluation failed")
                onEdt { callback.errorOccurred(why) }
                null
            }
    }

    /**
     * The answer arrives on the DAP client's thread, and the hint machinery is
     * EDT-only: it tracks the live hint so a mouse-move can dismiss it. A
     * callback delivered from another thread lands outside that bookkeeping,
     * and a popup shown that way is never told to close — which is exactly how
     * a hover popup outlives the mouse that summoned it (Julian, live
     * 2026-08-01).
     */
    private fun onEdt(block: () -> Unit) {
        val app = ApplicationManager.getApplication()
        when {
            app == null -> block()
            app.isDispatchThread -> block()
            else -> app.invokeLater(block, ModalityState.any())
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

    companion object {
        /**
         * How long a hover may take before its answer is no longer wanted
         * (Julian, live 2026-08-01). Deliberately short: the debuggee is
         * PARKED while we ask, so a healthy round trip is milliseconds, and
         * anything approaching this bound means something is wrong rather
         * than slow.
         */
        const val HOVER_TIMEOUT_SECONDS = 3L
    }

    override fun getEvaluationMode(
        text: String,
        startOffset: Int,
        endOffset: Int,
        psiFile: com.intellij.psi.PsiFile?,
    ): EvaluationMode = EvaluationMode.EXPRESSION
}
