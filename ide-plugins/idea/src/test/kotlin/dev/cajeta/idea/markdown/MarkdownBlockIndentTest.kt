package dev.cajeta.idea.markdown

import com.intellij.testFramework.PlatformTestUtil
import com.intellij.testFramework.fixtures.BasePlatformTestCase

/**
 * A rendered markdown block is indented to the scope of the code it documents.
 *
 * A `CustomFoldRegion` always starts painting at the left edge of the text area,
 * so a comment nested inside a class/method used to render flush left while the
 * code around it sat several levels in. The indent is captured from the source
 * (tabs expanded to the editor's tab size) and applied by [MarkdownFoldRenderer]
 * as a paint offset plus a matching width reservation.
 */
class MarkdownBlockIndentTest : BasePlatformTestCase() {

    fun testIndentColumnsFromSpaces() {
        val text = "class C {\n    // doc\n    int32 x = 1;\n}\n"
        val commentStart = text.indexOf("//")
        assertEquals(
            4,
            MarkdownFoldEditorListener.indentColumnsAt(text, commentStart, tabSize = 4),
        )
    }

    fun testIndentColumnsExpandTabs() {
        val text = "class C {\n\t// doc\n\tint32 x = 1;\n}\n"
        val commentStart = text.indexOf("//")
        assertEquals(
            "a tab counts as a full tab stop, not one column",
            4,
            MarkdownFoldEditorListener.indentColumnsAt(text, commentStart, tabSize = 4),
        )
    }

    fun testTopLevelCommentHasNoIndent() {
        val text = "// doc\nint32 x = 1;\n"
        assertEquals(0, MarkdownFoldEditorListener.indentColumnsAt(text, 0, tabSize = 4))
    }

    /** The installed block carries the indent of the source comment. */
    fun testInstalledBlockCarriesSourceIndent() {
        myFixture.configureByText(
            "Demo.cajeta",
            "class C {\n    // # Doc\n    // body\n    int32 x = 1;\n}\n",
        )
        PlatformTestUtil.dispatchAllInvocationEventsInIdeEventQueue()

        val block = MarkdownFoldEditorListener.wholeLineBlocksFor(myFixture.editor).single()
        assertEquals(
            "block nested one level in should render indented, not flush left",
            4, block.indentColumns,
        )
    }

    override fun tearDown() {
        try {
            // The pure indent tests never open a file, so there is no editor.
            @Suppress("UNNECESSARY_SAFE_CALL")
            myFixture?.editor?.let {
                MarkdownFoldEditorListener.uninstall(it)
                PlatformTestUtil.dispatchAllInvocationEventsInIdeEventQueue()
            }
        } finally {
            super.tearDown()
        }
    }
}
