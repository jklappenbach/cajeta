package dev.cajeta.idea.markdown

import com.intellij.openapi.diagnostic.Logger
import com.intellij.openapi.editor.Editor
import com.intellij.openapi.editor.EditorFactory
import com.intellij.openapi.editor.FoldRegion
import com.intellij.openapi.editor.event.CaretEvent
import com.intellij.openapi.editor.event.CaretListener
import com.intellij.openapi.editor.event.EditorMouseEvent
import com.intellij.openapi.editor.event.EditorMouseListener
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
    }
}

private object CommentMouseListener : EditorMouseListener {

    private val log = Logger.getInstance(CommentMouseListener::class.java)

    override fun mouseClicked(event: EditorMouseEvent) {
        val editor = event.editor
        val file = FileDocumentManager.getInstance().getFile(editor.document) ?: return
        if (file.fileType != CajetaFileType) return
        if (!CajetaSettings.instance.renderMarkdownInComments) return

        val offset = event.offset

        // Whole-line markdown block clicked? CustomFoldRegion can't be
        // toggled via isExpanded, so we remove the region to reveal
        // the source. The caret listener will re-collapse when the
        // caret leaves the block.
        val block = MarkdownFoldEditorListener.findBlockAt(editor, offset)
        if (block != null && block.isCollapsed) {
            log.debug("mouseClicked: expand whole-line block at offset=$offset range=${block.startOffset}-${block.endOffset}")
            MarkdownFoldEditorListener.expandBlock(editor, block)
            return
        }

        // Trailing comment? Standard fold + isExpanded works for those.
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
