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

    /**
     * The mouse path consumes presses on rendered blocks, so the caret never
     * moves and the caret listener's re-collapse sweep never runs while the user
     * works inside blocks. mousePressed therefore calls collapseAllExcept
     * directly — pin that the sweep re-renders every expanded block except the
     * one being interacted with.
     */
    fun testCollapseAllExceptRerendersOtherBlocks() {
        myFixture.configureByText("AsyncDemo.cajeta", source)
        PlatformTestUtil.dispatchAllInvocationEventsInIdeEventQueue()
        val editor = myFixture.editor

        val blocks = MarkdownFoldEditorListener.wholeLineBlocksFor(editor)
        assertEquals(3, blocks.size)

        // Open two blocks (as a failed install or a double-click would).
        MarkdownFoldEditorListener.expandBlock(editor, blocks[0])
        MarkdownFoldEditorListener.expandBlock(editor, blocks[1])
        assertFalse(blocks[0].isCollapsed)
        assertFalse(blocks[1].isCollapsed)

        // Attention shifts to blocks[1]: everything else re-renders, it stays open.
        MarkdownFoldEditorListener.collapseAllExcept(editor, blocks[1])
        assertTrue("non-active block must re-render", blocks[0].isCollapsed)
        assertFalse("active block must stay open", blocks[1].isCollapsed)
        assertTrue(blocks[2].isCollapsed)

        // No active block (caret in plain code): everything re-renders.
        MarkdownFoldEditorListener.collapseAllExcept(editor, null)
        assertTrue(blocks[1].isCollapsed)
    }

    /**
     * Install runs twice for editors open at project startup (editorCreated +
     * the project activity). The second pass used to skip runs whose region
     * already existed WITHOUT creating a state, then rebuild the state list
     * without them — leaving an orphan region that painted forever but was
     * unselectable, un-toggleable, and blocked every re-add on its range.
     * Install must be idempotent: after N passes, exactly one region per run,
     * each owned by a state.
     */
    fun testSecondInstallPassLeavesNoOrphanRegions() {
        myFixture.configureByText("AsyncDemo.cajeta", source)
        PlatformTestUtil.dispatchAllInvocationEventsInIdeEventQueue()
        val editor = myFixture.editor

        MarkdownFoldEditorListener.install(editor)   // the second pass
        PlatformTestUtil.dispatchAllInvocationEventsInIdeEventQueue()

        val blocks = MarkdownFoldEditorListener.wholeLineBlocksFor(editor)
        assertEquals("every run must survive the second pass", 3, blocks.size)

        val mdRegions = editor.foldingModel.allFoldRegions
            .filterIsInstance<com.intellij.openapi.editor.CustomFoldRegion>()
            .filter { it.renderer is MarkdownFoldRenderer }
        assertEquals("one region per block, no orphans", 3, mdRegions.size)

        for (region in mdRegions) {
            assertNotNull(
                "region ${region.startOffset}-${region.endOffset} must map back to a state",
                MarkdownFoldEditorListener.blockForRegion(editor, region),
            )
        }
    }

    /**
     * The press-time sweep must not re-render blocks ABOVE the press point: an
     * open block above changing height shifts the pressed block away from the
     * pointer between the two presses of a double-click, killing the expand.
     * Blocks below can't move anything above them, so they are swept.
     */
    fun testCollapseBelowLeavesBlocksAbovePressUntouched() {
        myFixture.configureByText("AsyncDemo.cajeta", source)
        PlatformTestUtil.dispatchAllInvocationEventsInIdeEventQueue()
        val editor = myFixture.editor

        val blocks = MarkdownFoldEditorListener.wholeLineBlocksFor(editor)
        assertEquals(3, blocks.size)
        MarkdownFoldEditorListener.expandBlock(editor, blocks[0])
        MarkdownFoldEditorListener.expandBlock(editor, blocks[2])

        // Press lands on blocks[1]: sweep from its top edge.
        val pressY = editor.logicalPositionToXY(
            com.intellij.openapi.editor.LogicalPosition(blocks[1].startLine, 0)).y
        MarkdownFoldEditorListener.collapseBelow(editor, blocks[1], pressY)

        assertFalse("block above the press must stay open (no layout shift)", blocks[0].isCollapsed)
        assertTrue("block below the press must re-render", blocks[2].isCollapsed)
    }

    /**
     * The deferred sweep re-renders blocks the press-time sweep must skip
     * (those above the click), once the double-click window has passed — and a
     * new press cancels it, so it can never fire mid-double-click.
     */
    fun testDeferredSweepFiresAfterClickWindowAndCancelOnPress() {
        myFixture.configureByText("AsyncDemo.cajeta", source)
        PlatformTestUtil.dispatchAllInvocationEventsInIdeEventQueue()
        val editor = myFixture.editor

        val blocks = MarkdownFoldEditorListener.wholeLineBlocksFor(editor)
        MarkdownFoldEditorListener.expandBlock(editor, blocks[0])

        // Cancel path: scheduled, then a new press arrives — must not fire.
        MarkdownFoldEditorListener.scheduleSweepAfterClickWindow(editor, blocks[1])
        MarkdownFoldEditorListener.cancelScheduledSweep(editor)
        Thread.sleep(900)
        PlatformTestUtil.dispatchAllInvocationEventsInIdeEventQueue()
        assertFalse("canceled sweep must not re-render", blocks[0].isCollapsed)

        // Fire path: scheduled and left alone — re-renders after the window.
        MarkdownFoldEditorListener.scheduleSweepAfterClickWindow(editor, blocks[1])
        val deadline = System.currentTimeMillis() + 5_000
        while (!blocks[0].isCollapsed && System.currentTimeMillis() < deadline) {
            Thread.sleep(50)
            PlatformTestUtil.dispatchAllInvocationEventsInIdeEventQueue()
        }
        assertTrue("deferred sweep must re-render the block above", blocks[0].isCollapsed)
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
