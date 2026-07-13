package dev.cajeta.idea.lint

import com.intellij.lang.annotation.AnnotationHolder
import com.intellij.lang.annotation.ExternalAnnotator
import com.intellij.lang.annotation.HighlightSeverity
import com.intellij.openapi.application.ApplicationManager
import com.intellij.psi.PsiFile
import dev.cajeta.idea.xref.CajetaXrefShards

data class LintInput(val path: String, val text: String)

class CajetaLintAnnotator : ExternalAnnotator<LintInput, LintOutput>() {

    override fun collectInformation(file: PsiFile): LintInput? {
        val path = file.virtualFile?.path ?: return null
        return LintInput(path, file.text)
    }

    // ide-symbol-index Unit 6 (6.2.4): the per-edit lint run now carries the
    // buffer's xref records on the same stderr (one subprocess, §1.5.2); the
    // stream costs ~2% of the lint wall time (Unit 3 numbers).
    override fun doAnnotate(input: LintInput): LintOutput =
        CajetacRunner.lintWithXref(input.path, input.text, emitXref = true)

    override fun apply(file: PsiFile, output: LintOutput, holder: AnnotationHolder) {
        for (d in output.diagnostics) {
            val severity = when (d.severity) {
                Diagnostic.Severity.ERROR -> HighlightSeverity.ERROR
                Diagnostic.Severity.WARNING -> HighlightSeverity.WARNING
                Diagnostic.Severity.WEAK_WARNING -> HighlightSeverity.WEAK_WARNING
            }
            holder.newAnnotation(severity, "[${d.ruleId}] ${d.message}")
                .range(d.range)
                .create()
        }

        // Feed the index off the EDT. A version-only stream (broken buffer)
        // has no records, so the previous shard is KEPT (spec 2.0.5); an
        // unsupported major was already refused wholesale at demux.
        if (output.xref.supported && output.xref.records.isNotEmpty()) {
            val project = file.project
            val records = output.xref.records
            ApplicationManager.getApplication().executeOnPooledThread {
                CajetaXrefShards.ingestStream(project, records)
            }
        }
    }
}
