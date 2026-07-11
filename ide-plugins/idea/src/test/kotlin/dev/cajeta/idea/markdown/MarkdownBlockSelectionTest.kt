package dev.cajeta.idea.markdown

import com.intellij.testFramework.fixtures.BasePlatformTestCase

/**
 * Selecting text inside a *rendered* markdown block.
 *
 * A folded block is invisible to the editor's selection model, so the text can't
 * be selected as ordinary editor text. The Swing surface lays out real text, so a
 * point maps to a document offset and a range can be highlighted and read back —
 * that is what click-drag over a block drives ([MarkdownSelectionController]) and
 * what Copy reads ([MarkdownCopyHandler]).
 *
 * These exercise the view's selection contract directly. The controller's
 * hit-testing needs a laid-out editor component (a fold region has no `location`
 * until it's painted), which a headless fixture doesn't give us — so the
 * pixel→region mapping is covered by the manual verification pass, and the
 * selection model itself is pinned here.
 */
class MarkdownBlockSelectionTest : BasePlatformTestCase() {

    private fun view(): SwingMarkdownBlockView {
        val html = MarkdownEngineRegistry.getInstance().active()
            .renderToHtml("Alpha beta gamma.")
        return SwingMarkdownBlockView(html)
    }

    fun testSwingSurfaceSupportsSelection() {
        assertTrue(
            "the Swing surface lays out real text, so it must back selection",
            view().supportsSelection,
        )
    }

    fun testJcefSurfaceDeclinesSelection() {
        // JCEF renders to a BufferedImage — there is no text model to select
        // against, so it must not claim selection support (the controller then
        // leaves those blocks on the double-click-to-open path).
        assertFalse(
            "an image-backed surface cannot support text selection",
            JcefMarkdownBlockView("<p>x</p>").supportsSelection,
        )
    }

    fun testSelectingARangeYieldsRenderedTextNotCommentSource() {
        myFixture.configureByText("Demo.cajeta", "// Alpha beta gamma.\nint32 x = 1;\n")
        val editor = myFixture.editor
        val view = view()

        // Lay the pane out, then select the whole rendered document.
        view.heightForWidth(editor, WIDTH, editor.lineHeight)
        view.setSelection(0, Int.MAX_VALUE)

        val selected = view.selectedText()
        assertNotNull("a non-empty range must yield text", selected)
        assertTrue(
            "selection yields the rendered text, not the `//` source (was: $selected)",
            selected!!.contains("Alpha beta gamma"),
        )
        assertFalse("comment markers are not part of the rendered text", selected.contains("//"))
    }

    fun testEmptySelectionYieldsNothing() {
        myFixture.configureByText("Demo.cajeta", "// Alpha beta gamma.\nint32 x = 1;\n")
        val view = view()
        view.heightForWidth(myFixture.editor, WIDTH, myFixture.editor.lineHeight)

        view.setSelection(3, 3)
        assertNull("an empty range is not a selection — Copy must fall through", view.selectedText())

        view.setSelection(0, 5)
        assertNotNull(view.selectedText())
        view.clearSelection()
        assertNull("clearSelection drops the highlight", view.selectedText())
    }

    fun testPointMapsToAnOffsetInsideTheBlock() {
        myFixture.configureByText("Demo.cajeta", "// Alpha beta gamma.\nint32 x = 1;\n")
        val view = view()
        val offset = view.offsetAt(myFixture.editor, WIDTH, px = 40, py = 8)
        assertTrue("a point inside the block maps to a real offset (was $offset)", offset >= 0)
    }

    private companion object {
        const val WIDTH = 400
    }
}
