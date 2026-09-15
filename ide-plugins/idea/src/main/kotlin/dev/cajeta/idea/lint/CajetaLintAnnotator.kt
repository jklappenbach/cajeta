package dev.cajeta.idea.lint

import com.intellij.lang.annotation.AnnotationHolder
import com.intellij.lang.annotation.ExternalAnnotator
import com.intellij.lang.annotation.HighlightSeverity
import com.intellij.openapi.application.ApplicationManager
import com.intellij.psi.PsiDocumentManager
import com.intellij.psi.PsiFile
import dev.cajeta.idea.xref.CajetaXrefPositions
import dev.cajeta.idea.xref.CajetaXrefShards

data class LintInput(val path: String, val text: String, val basePath: String?,
                     val stamp: Long = 0L)

class CajetaLintAnnotator : ExternalAnnotator<LintInput, LintOutput>() {

    override fun collectInformation(file: PsiFile): LintInput? {
        val path = file.virtualFile?.path ?: return null
        val stamp = PsiDocumentManager.getInstance(file.project)
            .getDocument(file)?.modificationStamp ?: 0L
        return LintInput(path, file.text, file.project.basePath, stamp)
    }

    // ide-symbol-index Unit 6 (6.2.4): the per-edit lint run now carries the
    // buffer's xref records on the same stderr (one subprocess, §1.5.2); the
    // stream costs ~2% of the lint wall time (Unit 3 numbers). The project base
    // rides along so the run resolves dependency archives — otherwise this
    // stream clobbers the whole-root shard's dependency Ctrl-click targets.
    override fun doAnnotate(input: LintInput): LintOutput =
        CajetacRunner.lintWithXref(input.path, input.text, emitXref = true,
                                   basePath = input.basePath)
            .copy(sourceStamp = input.stamp)

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
        // Unit 4: an unbuilt or stale own archive degrades resolution in a way
        // that looks IDENTICAL to the bug this spec fixed — the project's own
        // types stop resolving — so say which it is rather than leaving the
        // developer to guess from red underlines.
        val archive =
            if (configured)
                dev.cajeta.idea.xref.CajetaSourceMountGlue
                    .archiveHealth(compilerPath, file.project.basePath)
            else dev.cajeta.idea.xref.ArchiveHealth.State.OK
        dev.cajeta.idea.xref.CajetaXrefFreshness.getInstance(file.project)
            .updateFromLint(configured, output.xref, archive)

        // Feed the index off the EDT. A version-only stream (broken buffer)
        // has no records, so the previous shard is KEPT (spec 2.0.5); an
        // unsupported major was already refused wholesale at demux.
        if (output.xref.supported && output.xref.records.isNotEmpty()) {
            val project = file.project
            val records = output.xref.records
            ApplicationManager.getApplication().executeOnPooledThread {
                CajetaXrefShards.ingestStream(project, records)
            }
            // Pin the use positions to the document only if it still holds the
            // linted text; an edit that landed meanwhile is caught by the next lint.
            val doc = PsiDocumentManager.getInstance(project).getDocument(file)
            if (doc != null && doc.modificationStamp == output.sourceStamp) {
                CajetaXrefPositions.anchor(doc, records)
            }
        }
    }
}
