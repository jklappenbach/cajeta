package dev.cajeta.idea.markdown

import com.intellij.testFramework.PlatformTestUtil
import com.intellij.testFramework.fixtures.BasePlatformTestCase

/**
 * Every rendered block is clickable — not just the one at column 0.
 *
 * The regression: clicks were resolved with `findBlockAt(event.offset)`. A
 * `CustomFoldRegion` paints a block much taller than the collapsed text it stands
 * in for, so a click inside an *indented* block resolved to an offset outside the
 * folded range and matched nothing — leaving every comment nested in a class or
 * method dead to double-click while the top-of-file one still worked. Clicks are
 * now resolved from the region's painted bounds, and the region is mapped back to
 * its block by identity ([MarkdownFoldEditorListener.blockForRegion]).
 *
 * The pixel hit-test itself needs a laid-out editor (a fold region has no
 * `location` until painted), which a headless fixture doesn't provide — so what
 * is pinned here is the part that was silently wrong: that every block, at every
 * indent, is installed with a region that maps back to it.
 */
class MarkdownBlockClickTest : BasePlatformTestCase() {

    private val source = """
        package tour.concurrent;

        // Top comment at column zero.
        // Second line of it.
        public class AsyncDemo extends DemoClass {

            // Nested doc comment, indented one level.
            // It renders, so it must also be clickable.
            public static async int32 leaf() {
                return 21;
            }

            // Another nested doc comment.
            public static async int32 add(int32 a, int32 b) {
                return a + b;
            }
        }
    """.trimIndent()

    fun testEveryBlockIndentedOrNotMapsBackFromItsRegion() {
        myFixture.configureByText("AsyncDemo.cajeta", source)
        PlatformTestUtil.dispatchAllInvocationEventsInIdeEventQueue()
        val editor = myFixture.editor

        val blocks = MarkdownFoldEditorListener.wholeLineBlocksFor(editor)
        assertEquals("one top-level block + two nested ones", 3, blocks.size)

        // The nested ones are exactly what the bug killed.
        val indents = blocks.map { it.indentColumns }
        assertEquals("blocks are at column 0, 4, 4", listOf(0, 4, 4), indents)

        for (block in blocks) {
            assertTrue("block at indent ${block.indentColumns} must be collapsed", block.isCollapsed)
            val region = block.customRegion
            assertNotNull("block at indent ${block.indentColumns} must have a painted region", region)
            assertSame(
                "region must map back to its own block (indent ${block.indentColumns}) — " +
                    "this is what a click resolves through now",
                block,
                MarkdownFoldEditorListener.blockForRegion(editor, region!!),
            )
        }
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
