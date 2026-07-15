package dev.cajeta.idea.markdown

import com.intellij.openapi.editor.CustomFoldRegion
import com.intellij.openapi.editor.Editor
import com.intellij.openapi.util.Key
import java.awt.Point

/**
 * Text selection over a *rendered* markdown block — click-drag to highlight, then
 * Copy (see [MarkdownCopyHandler]) to take the text.
 *
 * A `CustomFoldRegion` is opaque to the editor: the folded source is hidden, and
 * the painted block is not part of the editor's text model, so the editor's own
 * selection can't reach it. That's why selecting rendered markdown needs its own
 * controller. Hit-testing goes through the region's painted bounds
 * ([CustomFoldRegion.getLocation] + width/height, both in content-component
 * coordinates — the same space the mouse event arrives in), then through the
 * renderer's indent to the block-local point the view can map to an offset.
 *
 * Only the Swing surface backs this: it lays out real text, so a pixel maps to a
 * character. The JCEF surface paints a `BufferedImage` and has no text model, so
 * [MarkdownFoldRenderer.selectableView] returns null for it and this controller
 * declines — those blocks keep the old behavior (double-click to open the
 * source, which is selectable as ordinary editor text).
 *
 * At most one block is selected at a time, per editor.
 */
internal object MarkdownSelectionController {

    private class Selection(
        val region: CustomFoldRegion,
        val anchor: Int,
        var lead: Int,
    )

    private val SELECTION_KEY: Key<Selection> =
        Key.create("dev.cajeta.idea.markdown.blockSelection")

    /** The collapsed markdown block painted under [point], if any. */
    fun blockRegionAt(editor: Editor, point: Point): CustomFoldRegion? =
        MarkdownFoldEditorListener.wholeLineBlocksFor(editor)
            .asSequence()
            .mapNotNull { it.customRegion }
            .filter { it.isValid }
            .firstOrNull { region ->
                val loc = region.location ?: return@firstOrNull false
                point.x >= loc.x && point.x <= loc.x + region.widthInPixels &&
                    point.y >= loc.y && point.y <= loc.y + region.heightInPixels
            }

    /**
     * Begin a selection at [point] inside [region]. Returns false when the
     * region's surface can't be selected (JCEF), so the caller can fall through
     * to the old click behavior.
     */
    fun beginSelection(editor: Editor, region: CustomFoldRegion, point: Point): Boolean {
        val offset = offsetAt(editor, region, point) ?: return false
        clearSelection(editor)
        editor.putUserData(SELECTION_KEY, Selection(region, offset, offset))
        // A press with no drag yet is an empty selection — this also clears any
        // stale highlight in the block the user just clicked into.
        applyHighlight(editor, region, offset, offset)
        return true
    }

    /** Extend the in-progress selection to [point]. No-op if none is in progress. */
    fun dragTo(editor: Editor, point: Point): Boolean {
        val selection = editor.getUserData(SELECTION_KEY) ?: return false
        if (!selection.region.isValid) {
            clearSelection(editor)
            return false
        }
        // Clamp to the origin region: dragging past the block's edge extends to
        // its start/end rather than jumping to whatever block is under the cursor.
        val offset = offsetAt(editor, selection.region, point) ?: return false
        selection.lead = offset
        applyHighlight(editor, selection.region, selection.anchor, offset)
        return true
    }

    /** The selected rendered text, or null when there is no (non-empty) selection. */
    fun selectedText(editor: Editor): String? {
        val selection = editor.getUserData(SELECTION_KEY) ?: return null
        if (!selection.region.isValid) return null
        return view(selection.region)?.selectedText()
    }

    fun hasSelection(editor: Editor): Boolean = selectedText(editor) != null

    /** Drop the highlight and forget the selection. Safe to call unconditionally. */
    fun clearSelection(editor: Editor) {
        val selection = editor.getUserData(SELECTION_KEY) ?: return
        editor.putUserData(SELECTION_KEY, null)
        val region = selection.region
        if (!region.isValid) return
        view(region)?.clearSelection()
        region.repaint()
    }

    private fun applyHighlight(editor: Editor, region: CustomFoldRegion, from: Int, to: Int) {
        view(region)?.setSelection(from, to) ?: return
        region.repaint()
    }

    private fun view(region: CustomFoldRegion): MarkdownBlockView? =
        (region.renderer as? MarkdownFoldRenderer)?.selectableView()

    /**
     * Editor-space [point] → offset in the region's rendered document. Null when
     * the region isn't a markdown block, its surface can't be selected, or it
     * isn't currently laid out (no location).
     */
    private fun offsetAt(editor: Editor, region: CustomFoldRegion, point: Point): Int? {
        val renderer = region.renderer as? MarkdownFoldRenderer ?: return null
        val view = renderer.selectableView() ?: return null
        val loc = region.location ?: return null
        val indent = renderer.indentPx(editor)
        val localX = (point.x - loc.x - indent).coerceAtLeast(0)
        val localY = (point.y - loc.y).coerceAtLeast(0)
        return view.offsetAt(editor, renderer.bodyWidth(region), localX, localY)
            .takeIf { it >= 0 }
    }
}
