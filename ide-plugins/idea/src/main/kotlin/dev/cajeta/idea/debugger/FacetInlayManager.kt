package dev.cajeta.idea.debugger

import com.intellij.codeInsight.daemon.impl.HintRenderer
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.editor.EditorFactory
import com.intellij.openapi.editor.Inlay
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.openapi.project.Project
import com.intellij.xdebugger.XSourcePosition

/**
 * Owns the inline editor decoration that shows the memory facets of the
 * bindings active at the current stop on the page itself (CP7-4, FR-6.1).
 *
 * An after-line-end inlay hint on the stopped top frame's line, text built by
 * the pure [inlineHint] from the SAME [DapVariable] facets the Variables view
 * and gutter use (single source of truth, FR-6.5). One inlay per open editor of
 * the file; [showAt] replaces any prior hint, [clear] removes them, and the
 * process clears on step / resume / session end so the hint never goes stale
 * (FR-6.3). All inlay mutation is marshalled onto the EDT.
 */
class FacetInlayManager(private val project: Project) {

    private val inlays = mutableListOf<Inlay<*>>()

    /** Show (or replace) the inline hint on [position]'s line for [vars]. */
    fun showAt(position: XSourcePosition, vars: List<DapVariable>) {
        val text = inlineHint(vars)
        ApplicationManager.getApplication().invokeLater {
            clearInternal()
            if (text == null) return@invokeLater
            val doc = FileDocumentManager.getInstance().getDocument(position.file)
                ?: return@invokeLater
            if (position.line < 0 || position.line >= doc.lineCount) return@invokeLater
            val offset = doc.getLineEndOffset(position.line)
            val renderer = HintRenderer("  $text")
            for (editor in EditorFactory.getInstance().getEditors(doc, project)) {
                editor.inlayModel.addAfterLineEndElement(offset, false, renderer)?.let { inlays.add(it) }
            }
        }
    }

    /** Remove the current hint(s), if any. Safe to call repeatedly. */
    fun clear() {
        ApplicationManager.getApplication().invokeLater { clearInternal() }
    }

    private fun clearInternal() {
        inlays.forEach { it ->
            try {
                it.dispose()
            } catch (_: Exception) {
                // Editor/document already gone: ignore.
            }
        }
        inlays.clear()
    }
}
