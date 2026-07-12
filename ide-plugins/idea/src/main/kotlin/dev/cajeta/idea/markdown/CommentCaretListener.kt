package dev.cajeta.idea.markdown

import com.intellij.openapi.diagnostic.Logger
import com.intellij.openapi.editor.CustomFoldRegion
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
/**
 * The block region under the most recent press, per editor. A double-click's
 * second press resolves its target through this instead of a fresh hit-test:
 * any re-render between the two presses shifts the layout, and the second
 * press's coordinates can land outside the (moved) region the user aimed at.
 */
private val LAST_PRESSED_BLOCK: com.intellij.openapi.util.Key<CustomFoldRegion> =
    com.intellij.openapi.util.Key.create("dev.cajeta.idea.markdown.lastPressedBlock")

private object CommentMouseListener : EditorMouseListener {

    private val log = Logger.getInstance(CommentMouseListener::class.java)

    override fun mousePressed(event: EditorMouseEvent) {
        val editor = event.editor
        if (!isRenderingCajetaEditor(editor)) return

        // A new press invalidates any sweep scheduled by the previous click —
        // it must never fire between the two presses of a double-click.
        MarkdownFoldEditorListener.cancelScheduledSweep(editor)

        val point = event.mouseEvent.point
        val region = MarkdownSelectionController.blockRegionAt(editor, point)
        // Double-click → open the source. Handled on the PRESS, not in
        // mouseClicked: presses on blocks are consumed, and a consumed press
        // suppresses the platform's clicked event — an expand handler there
        // never runs on a block. clickCount is already 2 on the second press,
        // so no clicked event is needed.
        if (event.mouseEvent.clickCount >= 2) {
            // Prefer the region recorded by this gesture's FIRST press: any
            // re-render between the two presses shifts the layout, and the
            // second press's hit-test can land outside the moved region or on
            // a neighbor. The recorded press is the user's intent, and unlike
            // a selection anchor it exists even when the first press hit a
            // non-text part of the block (padding, margins) or a JCEF block.
            val pressed = editor.getUserData(LAST_PRESSED_BLOCK)?.takeIf { it.isValid }
            if (pressed != null && pressed !== region) {
                log.warn("double-click hit-test moved off the pressed block (layout shift?); using the pressed block")
            }
            val target = pressed ?: region
            if (target == null) {
                MarkdownSelectionController.clearSelection(editor)
                return
            }
            val block = MarkdownFoldEditorListener.blockForRegion(editor, target) ?: return
            log.debug("mousePressed x2: expand whole-line block lines ${block.startLine}-${block.endLine}")
            MarkdownSelectionController.clearSelection(editor)
            // Caret FIRST, expand SECOND. The caret is often still parked
            // inside some previously re-collapsed block (consumed presses
            // never move it), and expandBlock's fold-model change can fire a
            // synchronous caret adjustment; if the caret is not inside THIS
            // block when that sweep runs, it re-collapses the block in the
            // same instant it was expanded — a double-click that visibly does
            // nothing, on exactly every other gesture. With the caret placed
            // first, any sweep keeps this block.
            editor.caretModel.moveToOffset(block.startOffset)
            MarkdownFoldEditorListener.expandBlock(editor, block)
            event.consume()
            return
        }
        if (region == null) {
            // Clicked away from every block — drop any highlight, and the
            // recorded press (so a stale one can't hijack a double-click on
            // plain text, e.g. word selection).
            editor.putUserData(LAST_PRESSED_BLOCK, null)
            MarkdownSelectionController.clearSelection(editor)
            return
        }
        editor.putUserData(LAST_PRESSED_BLOCK, region)
        // Try to anchor a selection (Swing surface). Consume REGARDLESS of
        // whether the surface supports selection: an unconsumed press would
        // move the caret, whose listener runs the full re-collapse sweep —
        // shifting the layout right in the middle of a double-click gesture.
        MarkdownSelectionController.beginSelection(editor, region, point)
        // A consumed press never moves the caret, so the caret listener's
        // re-collapse sweep can't run while the user works inside rendered
        // blocks. Re-render other open blocks — but only those BELOW the
        // press point: re-rendering one above changes its height and moves
        // this block out from under a double-click's second press. Blocks
        // above are swept after the click window (see mouseReleased).
        MarkdownFoldEditorListener.collapseBelow(
            editor, MarkdownFoldEditorListener.blockForRegion(editor, region), point.y)
        event.consume()
    }

    override fun mouseReleased(event: EditorMouseEvent) {
        val editor = event.editor
        if (!isRenderingCajetaEditor(editor)) return
        if (event.mouseEvent.clickCount != 1) return
        // Single click on a block completed: once the double-click window has
        // passed with no second press, re-render every other open block —
        // including those ABOVE the click, which the press-time sweep must
        // skip (their re-render would shift the layout mid-double-click).
        val region = editor.getUserData(LAST_PRESSED_BLOCK)?.takeIf { it.isValid } ?: return
        MarkdownFoldEditorListener.scheduleSweepAfterClickWindow(
            editor, MarkdownFoldEditorListener.blockForRegion(editor, region))
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
        //
        // This only fires for non-selectable (JCEF) blocks: a selectable block's
        // press is consumed by mousePressed, which suppresses clicked delivery —
        // its double-click is handled on the press itself.
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
        MarkdownFoldEditorListener.collapseAllExcept(editor, activeBlock)
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
