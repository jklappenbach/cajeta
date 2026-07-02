package dev.cajeta.idea.markdown

import com.intellij.testFramework.PlatformTestUtil
import com.intellij.testFramework.fixtures.BasePlatformTestCase

/**
 * Platform-fixture test for the Obsidian-style live-preview round-trip on
 * whole-line markdown blocks: a collapsed block renders markdown; "opening" it
 * (what a click does — [MarkdownFoldEditorListener.expandBlock]) removes the
 * fold so the raw source is present as ordinary editor text (hence editable /
 * selectable / copyable); moving the caret out of the block re-collapses it back
 * to the rendered view via [CommentCaretListener].
 */
class MarkdownLivePreviewTest : BasePlatformTestCase() {

    fun testExpandRevealsPlainSourceAndCaretLeaveReCollapses() {
        // Two contiguous whole-line comments → one collapsed CustomFoldRegion.
        myFixture.configureByText(
            "Demo.cajeta",
            "// # Heading\n// body text\nint32 x = 1;\n",
        )
        val editor = myFixture.editor

        // Startup activity installs + collapses the block via invokeLater.
        PlatformTestUtil.dispatchAllInvocationEventsInIdeEventQueue()

        val block = MarkdownFoldEditorListener.wholeLineBlocksFor(editor).single()
        assertTrue("block should start collapsed (rendered)", block.isCollapsed)
        assertNotNull(
            "a collapsed fold should cover the block's source while rendered",
            editor.foldingModel.getCollapsedRegionAtOffset(block.startOffset),
        )

        // "Click on the markdown" → expand to raw source. Once the fold is gone
        // the comment lines are ordinary document text: editable/selectable/
        // copyable with no special handling.
        MarkdownFoldEditorListener.expandBlock(editor, block)
        PlatformTestUtil.dispatchAllInvocationEventsInIdeEventQueue()

        assertFalse("block should be expanded after opening", block.isCollapsed)
        assertNull(
            "no collapsed fold should cover the source once expanded",
            editor.foldingModel.getCollapsedRegionAtOffset(block.startOffset),
        )

        // Caret leaves the block (e.g. a click outside) → re-render.
        editor.caretModel.moveToOffset(editor.document.textLength)
        PlatformTestUtil.dispatchAllInvocationEventsInIdeEventQueue()

        assertTrue("block should re-collapse (render) once the caret leaves", block.isCollapsed)
        assertNotNull(
            "a collapsed fold should cover the block again after re-render",
            editor.foldingModel.getCollapsedRegionAtOffset(block.startOffset),
        )
    }

    override fun tearDown() {
        try {
            MarkdownFoldEditorListener.uninstall(myFixture.editor)
            PlatformTestUtil.dispatchAllInvocationEventsInIdeEventQueue()
        } finally {
            super.tearDown()
        }
    }
}
