package dev.cajeta.idea.markdown

import com.intellij.openapi.diagnostic.Logger
import com.intellij.openapi.editor.Editor
import com.intellij.openapi.editor.EditorFactory
import com.intellij.openapi.editor.FoldRegion
import com.intellij.openapi.editor.event.CaretEvent
import com.intellij.openapi.editor.event.CaretListener
import com.intellij.openapi.editor.event.EditorMouseEvent
import com.intellij.openapi.editor.event.EditorMouseListener
import com.intellij.openapi.editor.event.EditorMouseMotionListener
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.openapi.project.Project
import com.intellij.openapi.startup.ProjectActivity
import dev.cajeta.idea.CajetaFileType
import dev.cajeta.idea.settings.CajetaSettings

/**
 * Obsidian live-preview behavior. When the caret enters a folded
 * comment region, expand it (raw source visible). When the caret
 * leaves, re-collapse it (rendered placeholder visible).
 *
 * Installs once per project at startup; the listener is
 * application-wide because EditorFactory's multicaster is
 * application-scoped, but it only operates on Cajeta files.
 */
class InstallCommentCaretListener : ProjectActivity {

    override suspend fun execute(project: Project) {
        val multicaster = EditorFactory.getInstance().eventMulticaster
        multicaster.addCaretListener(CommentCaretListener, project)
        // Backup: caret events do not always fire when clicking on
        // a CustomFoldRegion (the click can be absorbed by the
        // custom-painted area without moving the caret). The mouse
        // listener catches that path explicitly.
        multicaster.addEditorMouseListener(CommentMouseListener, project)
        // Drag over a rendered block selects its text (MarkdownSelectionController).
        multicaster.addEditorMouseMotionListener(CommentMouseMotionListener, project)
    }
}

/**
 * Mouse behavior over a rendered markdown block:
 *
 *  - **press + drag** → highlight the rendered text (copyable — see
 *    [MarkdownCopyHandler]). The block stays rendered.
 *  - **double-click** → replace the block with its raw source for editing.
 *
 * A single click without a drag does nothing but clear any highlight, so reading
 * a comment no longer flips it to source the moment you click near it. Trailing
 * comments have no rendered text model to select against (they are painted with
 * `drawString`), so a single click still opens those.
 */
private object CommentMouseListener : EditorMouseListener {

    private val log = Logger.getInstance(CommentMouseListener::class.java)

    override fun mousePressed(event: EditorMouseEvent) {
        val editor = event.editor
        if (!isRenderingCajetaEditor(editor)) return

        val point = event.mouseEvent.point
        val region = MarkdownSelectionController.blockRegionAt(editor, point)
        if (region == null) {
            // Clicked away from every block — drop any highlight.
            MarkdownSelectionController.clearSelection(editor)
            return
        }
        // Anchor a selection here. Consume so the editor doesn't also start its
        // own (meaningless) drag-selection across the folded region.
        if (MarkdownSelectionController.beginSelection(editor, region, point)) {
            event.consume()
        }
    }

    override fun mouseClicked(event: EditorMouseEvent) {
        val editor = event.editor
        if (!isRenderingCajetaEditor(editor)) return

        val offset = event.offset
        val doubleClick = event.mouseEvent.clickCount >= 2

        // Whole-line markdown block. Resolved by the region's painted bounds, NOT
        // by event.offset — a click inside a CustomFoldRegion does not reliably
        // map back into the folded range (the painted block is much taller than
        // the text it replaces), which left every indented block unclickable.
        // CustomFoldRegion can't be toggled via isExpanded, so we remove the
        // region to reveal the source; the caret listener re-collapses when the
        // caret leaves the block.
        val blockRegion = MarkdownSelectionController.blockRegionAt(editor, event.mouseEvent.point)
        if (blockRegion != null) {
            if (!doubleClick) return   // single click selects, it does not open
            val block = MarkdownFoldEditorListener.blockForRegion(editor, blockRegion) ?: return
            log.debug("mouseClicked x2: expand whole-line block lines ${block.startLine}-${block.endLine}")
            MarkdownSelectionController.clearSelection(editor)
            MarkdownFoldEditorListener.expandBlock(editor, block)
            event.consume()
            return
        }

        // Trailing comment? Standard fold + isExpanded works for those. These are
        // drawn glyph-by-glyph with no selectable text model, so a single click
        // still opens them — there's nothing to select instead.
        val region = editor.foldingModel.allFoldRegions
            .firstOrNull { it.startOffset <= offset && offset <= it.endOffset }
            ?: return
        if (region.isExpanded) return
        val isTrailing = MarkdownFoldEditorListener.isTrailingFold(editor, region)
        if (!isTrailing) return  // unknown fold; leave alone

        log.debug("mouseClicked: expanding trailing fold at offset=$offset")
        editor.foldingModel.runBatchFoldingOperation {
            region.isExpanded = true
            MarkdownFoldEditorListener.onTrailingFoldExpanded(editor, region)
        }
        CommentCaretListener.noteExpandedExternally(editor, region)
    }
}

private object CommentMouseMotionListener : EditorMouseMotionListener {

    override fun mouseDragged(event: EditorMouseEvent) {
        val editor = event.editor
        if (!isRenderingCajetaEditor(editor)) return
        if (MarkdownSelectionController.dragTo(editor, event.mouseEvent.point)) {
            event.consume()
        }
    }
}

private fun isRenderingCajetaEditor(editor: Editor): Boolean {
    val file = FileDocumentManager.getInstance().getFile(editor.document) ?: return false
    if (file.fileType != CajetaFileType) return false
    return CajetaSettings.instance.renderMarkdownInComments
}

internal object CommentCaretListener : CaretListener {

    private val log = Logger.getInstance(CommentCaretListener::class.java)

    /** The fold region currently expanded due to caret presence. */
    private var expandedRegion: FoldRegion? = null
    private var expandedEditor: Editor? = null

    /** Called by the mouse listener when it expands a fold on click. */
    fun noteExpandedExternally(editor: Editor, region: FoldRegion) {
        expandedRegion = region
        expandedEditor = editor
    }

    override fun caretPositionChanged(event: CaretEvent) {
        if (!CajetaSettings.instance.renderMarkdownInComments) {
            restorePreviouslyExpanded()
            return
        }

        val editor = event.editor
        if (!isCajetaEditor(editor)) return

        val caret = event.caret ?: return
        val offset = caret.offset

        // Moving the caret (arrow keys, a click elsewhere) abandons any
        // rendered-block highlight, the same way it would drop a text selection.
        MarkdownSelectionController.clearSelection(editor)

        // Whole-line blocks: re-collapse any block the caret has just
        // left. (CustomFoldRegion can't be toggled, so we have to
        // recreate the region by re-collapsing the BlockState.)
        val activeBlock = MarkdownFoldEditorListener.findBlockAt(editor, offset)
        for (block in MarkdownFoldEditorListener.wholeLineBlocksFor(editor)) {
            if (block !== activeBlock && !block.isCollapsed) {
                MarkdownFoldEditorListener.collapseBlock(editor, block)
            }
        }
        // If the caret has actually moved into an expanded block, do nothing
        // — the user is editing the source.

        val region = findCommentFoldAt(editor, offset)
        log.debug("caretPositionChanged offset=$offset region=${region?.let { "${it.startOffset}-${it.endOffset}" }} activeBlock=${activeBlock != null}")

        if (region == expandedRegion) return

        // Re-collapse the previously expanded fold (if any) and
        // expand the new one. Wrapped in a single batch so the
        // editor only repaints once. For trailing-comment folds,
        // also dispose / recreate the paired inlay so it doesn't
        // overlap with the now-visible source text.
        editor.foldingModel.runBatchFoldingOperation {
            restorePreviouslyExpanded()
            if (region != null && !region.isExpanded) {
                region.isExpanded = true
                expandedRegion = region
                expandedEditor = editor
                if (MarkdownFoldEditorListener.isTrailingFold(editor, region)) {
                    MarkdownFoldEditorListener.onTrailingFoldExpanded(editor, region)
                }
            }
        }
    }

    private fun restorePreviouslyExpanded() {
        val region = expandedRegion ?: return
        val editor = expandedEditor ?: return
        if (region.isValid && region.isExpanded) {
            editor.foldingModel.runBatchFoldingOperation {
                region.isExpanded = false
                if (MarkdownFoldEditorListener.isTrailingFold(editor, region)) {
                    MarkdownFoldEditorListener.onTrailingFoldCollapsed(editor, region)
                }
            }
        }
        expandedRegion = null
        expandedEditor = null
    }

    private fun isCajetaEditor(editor: Editor): Boolean {
        val file = FileDocumentManager.getInstance().getFile(editor.document) ?: return false
        return file.fileType == CajetaFileType
    }

    private fun findCommentFoldAt(editor: Editor, offset: Int): FoldRegion? {
        // The folding model only stores comment-language folds for
        // Cajeta files, so any region containing the offset is one
        // of ours. (Other plugins' regions would only land in non-
        // Cajeta editors.)
        return editor.foldingModel.allFoldRegions
            .firstOrNull { it.startOffset <= offset && offset <= it.endOffset }
    }
}
