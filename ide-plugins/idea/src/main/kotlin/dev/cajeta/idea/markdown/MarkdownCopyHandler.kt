package dev.cajeta.idea.markdown

import com.intellij.openapi.actionSystem.DataContext
import com.intellij.openapi.editor.Caret
import com.intellij.openapi.editor.Editor
import com.intellij.openapi.editor.actionSystem.EditorActionHandler
import com.intellij.openapi.ide.CopyPasteManager
import java.awt.datatransfer.StringSelection

/**
 * Routes Copy to the rendered markdown block when one is highlighted.
 *
 * A markdown block is a `CustomFoldRegion`, invisible to the editor's selection
 * model, so the editor sees no selection while a block is highlighted — and the
 * platform's Copy, finding nothing selected, would copy the caret's whole line
 * instead. This handler intercepts that case and puts the selected *rendered*
 * text (not the `//`-prefixed source) on the clipboard. With no block selection
 * it delegates untouched, so ordinary editor copy is unaffected.
 *
 * Registered on `EditorCopy` in plugin.xml.
 */
class MarkdownCopyHandler(private val original: EditorActionHandler) : EditorActionHandler() {

    override fun doExecute(editor: Editor, caret: Caret?, dataContext: DataContext?) {
        val selected = MarkdownSelectionController.selectedText(editor)
        if (selected != null) {
            CopyPasteManager.getInstance().setContents(StringSelection(selected))
            return
        }
        original.execute(editor, caret, dataContext)
    }

    override fun isEnabledForCaret(editor: Editor, caret: Caret, dataContext: DataContext?): Boolean =
        MarkdownSelectionController.hasSelection(editor) ||
            original.isEnabled(editor, caret, dataContext)
}
