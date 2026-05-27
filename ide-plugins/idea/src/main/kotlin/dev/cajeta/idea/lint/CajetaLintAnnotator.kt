package dev.cajeta.idea.lint

import com.intellij.lang.annotation.AnnotationHolder
import com.intellij.lang.annotation.ExternalAnnotator
import com.intellij.lang.annotation.HighlightSeverity
import com.intellij.psi.PsiFile

data class LintInput(val path: String, val text: String)

class CajetaLintAnnotator : ExternalAnnotator<LintInput, List<Diagnostic>>() {

    override fun collectInformation(file: PsiFile): LintInput? {
        val path = file.virtualFile?.path ?: return null
        return LintInput(path, file.text)
    }

    override fun doAnnotate(input: LintInput): List<Diagnostic> =
        CajetacRunner.lint(input.path, input.text)

    override fun apply(file: PsiFile, diagnostics: List<Diagnostic>, holder: AnnotationHolder) {
        for (d in diagnostics) {
            val severity = when (d.severity) {
                Diagnostic.Severity.ERROR -> HighlightSeverity.ERROR
                Diagnostic.Severity.WARNING -> HighlightSeverity.WARNING
                Diagnostic.Severity.WEAK_WARNING -> HighlightSeverity.WEAK_WARNING
            }
            holder.newAnnotation(severity, "[${d.ruleId}] ${d.message}")
                .range(d.range)
                .create()
        }
    }
}
