package dev.cajeta.idea.lint

import com.intellij.lang.annotation.AnnotationHolder
import com.intellij.lang.annotation.ExternalAnnotator
import com.intellij.lang.annotation.HighlightSeverity
import com.intellij.openapi.application.ApplicationManager
import com.intellij.psi.PsiFile
import dev.cajeta.idea.xref.CajetaXrefShards

data class LintInput(val path: String, val text: String, val basePath: String?)

class CajetaLintAnnotator : ExternalAnnotator<LintInput, LintOutput>() {

    override fun collectInformation(file: PsiFile): LintInput? {
        val path = file.virtualFile?.path ?: return null
        return LintInput(path, file.text, file.project.basePath)
    }

    // ide-symbol-index Unit 6 (6.2.4): the per-edit lint run now carries the
    // buffer's xref records on the same stderr (one subprocess, §1.5.2); the
    // stream costs ~2% of the lint wall time (Unit 3 numbers). The project base
    // rides along so the run resolves dependency archives — otherwise this
    // stream clobbers the whole-root shard's dependency Ctrl-click targets.
    override fun doAnnotate(input: LintInput): LintOutput =
        CajetacRunner.lintWithXref(input.path, input.text, emitXref = true,
                                   basePath = input.basePath)

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

        // Freshness (Unit 9): degradation is visible and reasoned — no
        // compiler / unknown schema major report themselves UNAVAILABLE
        // instead of failing silently.
        val compilerPath = dev.cajeta.idea.settings.CajetaSettings.instance.compilerPath
        val configured = compilerPath.isNotBlank() &&
            java.io.File(compilerPath).canExecute()
        dev.cajeta.idea.xref.CajetaXrefFreshness.getInstance(file.project)
            .updateFromLint(configured, output.xref)

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
