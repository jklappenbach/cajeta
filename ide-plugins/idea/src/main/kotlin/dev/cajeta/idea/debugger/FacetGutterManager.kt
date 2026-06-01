package dev.cajeta.idea.debugger

import com.intellij.icons.AllIcons
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.editor.impl.DocumentMarkupModel
import com.intellij.openapi.editor.markup.GutterIconRenderer
import com.intellij.openapi.editor.markup.HighlighterLayer
import com.intellij.openapi.editor.markup.MarkupModel
import com.intellij.openapi.editor.markup.RangeHighlighter
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.openapi.project.Project
import com.intellij.xdebugger.XSourcePosition
import javax.swing.Icon

/**
 * Owns the debug-session gutter icon that summarizes the memory facets of the
 * bindings active at the current stop (CP7-3, FR-6.2/6.3/6.5).
 *
 * One line-highlighter on the current execution line, its glyph + tooltip
 * derived by the pure [summarizeGutter] from the SAME [DapVariable] facets the
 * Variables view renders (single source of truth, FR-6.5). [showAt] replaces
 * any prior glyph; [clear] removes it. The process clears on step / resume /
 * session end so the gutter never shows stale state (FR-6.3). All markup
 * mutation is marshalled onto the EDT.
 */
class FacetGutterManager(private val project: Project) {

    private var current: RangeHighlighter? = null
    private var currentModel: MarkupModel? = null

    /** Show (or replace) the gutter glyph on [position]'s line for [vars]. */
    fun showAt(position: XSourcePosition, vars: List<DapVariable>) {
        val summary = summarizeGutter(vars)
        ApplicationManager.getApplication().invokeLater {
            clearInternal()
            if (summary == null) return@invokeLater
            val doc = FileDocumentManager.getInstance().getDocument(position.file)
                ?: return@invokeLater
            if (position.line < 0 || position.line >= doc.lineCount) return@invokeLater
            val model = DocumentMarkupModel.forDocument(doc, project, true)
            val hl = model.addLineHighlighter(null, position.line, HighlighterLayer.ADDITIONAL_SYNTAX)
            hl.gutterIconRenderer = CajetaFacetGutterRenderer(summary)
            current = hl
            currentModel = model
        }
    }

    /** Remove the current glyph, if any. Safe to call repeatedly. */
    fun clear() {
        ApplicationManager.getApplication().invokeLater { clearInternal() }
    }

    private fun clearInternal() {
        val hl = current ?: return
        try {
            currentModel?.removeHighlighter(hl)
        } catch (_: Exception) {
            // Document/model already gone (file closed, project disposed): ignore.
        }
        current = null
        currentModel = null
    }
}

/**
 * The gutter glyph for a [GutterSummary]: ownership picks the icon, a moved-out
 * binding on the line is flagged with a warning overlay-ish icon, and the
 * tooltip lists every binding's facet tag. equals/hashCode are required by
 * [GutterIconRenderer] so the platform can de-dupe identical renderers.
 */
class CajetaFacetGutterRenderer(private val summary: GutterSummary) : GutterIconRenderer() {

    override fun getIcon(): Icon = when {
        summary.anyMovedOut -> AllIcons.General.Warning
        summary.ownership == OwnershipRole.OWNER -> AllIcons.Nodes.Field
        summary.ownership == OwnershipRole.BORROW -> AllIcons.Nodes.Parameter
        else -> AllIcons.Debugger.Value
    }

    override fun getTooltipText(): String = summary.tooltip

    override fun equals(other: Any?): Boolean =
        other is CajetaFacetGutterRenderer && other.summary == summary

    override fun hashCode(): Int = summary.hashCode()
}
