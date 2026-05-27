package dev.cajeta.idea.harness

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.application.ReadAction
import com.intellij.openapi.application.invokeAndWaitIfNeeded
import com.intellij.openapi.command.WriteCommandAction
import com.intellij.openapi.diagnostic.Logger
import com.intellij.openapi.editor.Document
import com.intellij.openapi.editor.Editor
import com.intellij.openapi.editor.markup.HighlighterTargetArea
import com.intellij.openapi.editor.markup.RangeHighlighter
import com.intellij.openapi.project.Project
import com.intellij.openapi.editor.impl.DocumentMarkupModel
import com.intellij.lang.annotation.HighlightSeverity
import com.intellij.openapi.editor.markup.MarkupModel
import com.intellij.openapi.progress.ProgressIndicator
import com.intellij.openapi.progress.ProgressManager
import com.intellij.openapi.progress.Task

/**
 * Types a fixture into an editor character-by-character, polling
 * the markup model for parse errors at each step. Runs the typing
 * loop on a background thread (so the EDT stays responsive); each
 * character insertion is dispatched back to the EDT inside a write
 * action.
 */
class FixtureTyper(
    private val project: Project,
    private val editor: Editor,
    private val fixture: Fixture,
    private val delayMs: Int,
) {

    private val log = Logger.getInstance(FixtureTyper::class.java)
    private val document: Document = editor.document

    fun runUnderProgress() {
        ProgressManager.getInstance().run(object : Task.Backgroundable(
            project, "Typing fixture: ${fixture.displayName}", true,
        ) {
            override fun run(indicator: ProgressIndicator) {
                type(indicator)
            }
        })
    }

    private fun type(indicator: ProgressIndicator) {
        val text = fixture.text()
        val total = text.length
        val report = Report(fixture)

        clearDocument()
        indicator.text = "Typing ${fixture.displayName} (0 / $total)"

        for ((i, ch) in text.withIndex()) {
            indicator.checkCanceled()
            insertCharAtEnd(ch.toString())

            if (i % 32 == 0) {
                indicator.text = "Typing ${fixture.displayName} ($i / $total)"
                indicator.fraction = i.toDouble() / total
            }

            // Sample errors at every step. The most informative state
            // for catching transient ANTLR recovery bugs.
            val errors = countErrorHighlights()
            report.observe(i + 1, ch, errors)

            sleep(delayMs.toLong())
        }

        indicator.text = "Settling ${fixture.displayName}"
        sleep(200)  // let final annotations settle
        report.finalErrors = countErrorHighlights()
        report.log(log)
    }

    private fun clearDocument() {
        invokeAndWaitIfNeeded {
            WriteCommandAction.runWriteCommandAction(project) {
                document.setText("")
            }
        }
    }

    private fun insertCharAtEnd(s: String) {
        invokeAndWaitIfNeeded {
            WriteCommandAction.runWriteCommandAction(project) {
                document.insertString(document.textLength, s)
            }
        }
    }

    private fun countErrorHighlights(): Int {
        return ReadAction.compute<Int, RuntimeException> {
            val model: MarkupModel = DocumentMarkupModel.forDocument(document, project, true)
            model.allHighlighters.count { isErrorHighlight(it) }
        }
    }

    private fun isErrorHighlight(h: RangeHighlighter): Boolean {
        // Annotation severities are stored as user data on highlighters
        // by the inspection/annotation infrastructure. The cleanest
        // proxy is the highlight layer: ERROR-level highlights live
        // on layer >= HighlighterLayer.ERROR.
        // Filter common false positives (carets, selection markers)
        // by requiring target area = EXACT_RANGE.
        if (h.targetArea != HighlighterTargetArea.EXACT_RANGE) return false
        val info = HighlightSeverity::class.java
        // Approximate: use layer > 5000 (ERROR is 5800).
        return h.layer >= 5800
    }

    private fun sleep(ms: Long) {
        try {
            Thread.sleep(ms.coerceAtLeast(1))
        } catch (e: InterruptedException) {
            Thread.currentThread().interrupt()
            throw RuntimeException(e)
        }
    }

    /**
     * Per-character observation log. For `valid` fixtures, any
     * non-zero entry is a bug (the parser shouldn't error on a
     * well-formed intermediate state). For `invalid` fixtures we
     * just record where errors first appeared.
     */
    private class Report(val fixture: Fixture) {
        private val events = mutableListOf<Event>()
        private var lastErrorCount = 0
        var finalErrors: Int = 0

        data class Event(val charsTyped: Int, val ch: Char, val errors: Int)

        fun observe(charsTyped: Int, ch: Char, errors: Int) {
            if (errors != lastErrorCount) {
                events += Event(charsTyped, ch, errors)
                lastErrorCount = errors
            }
        }

        fun log(log: Logger) {
            log.warn("=== Typing report for ${fixture.displayName} (${fixture.kind}) ===")
            if (events.isEmpty()) {
                log.warn("  No error-count transitions during typing.")
            } else {
                log.warn("  Error-count transitions:")
                for (e in events) {
                    val chDisp = if (e.ch == '\n') "\\n" else e.ch.toString()
                    log.warn("    at char ${e.charsTyped} ('${chDisp}'): errors -> ${e.errors}")
                }
            }
            log.warn("  Final error count: $finalErrors")
            val verdict = when (fixture.kind) {
                Fixture.Kind.VALID -> if (finalErrors == 0 && events.isEmpty()) "PASS" else "FAIL"
                Fixture.Kind.INVALID -> if (finalErrors > 0) "PASS (errors as expected)" else "FAIL (expected errors)"
            }
            log.warn("  Verdict: $verdict")
        }
    }
}
