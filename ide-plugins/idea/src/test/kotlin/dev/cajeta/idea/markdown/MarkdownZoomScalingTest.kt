package dev.cajeta.idea.markdown

import com.intellij.openapi.editor.ex.EditorEx
import com.intellij.testFramework.PlatformTestUtil
import com.intellij.testFramework.fixtures.BasePlatformTestCase
import dev.cajeta.idea.settings.CajetaSettings

/**
 * Platform-fixture regression guard: rendered in-comment markdown scales with
 * editor zoom (Ctrl+wheel / Ctrl+=).
 *
 * The block is painted by a [CustomFoldRegion] whose size the editor re-measures
 * on a font-size change; the render surface derives its font from the zoom-aware
 * `editor.colorsScheme.editorFontSize` (via [EditorMarkdownPalette]). Together
 * that makes the rendered block grow with the surrounding code. This test drives
 * a real editor through a zoom and asserts the fold region's height tracks it —
 * so a regression that reads the font from the global scheme again (the pre-fix
 * bug), or otherwise decouples the render from zoom, is caught.
 *
 * The assertion is on the fold region's reported height, whose floor is the
 * editor line height ([MarkdownFoldRenderer.calcHeightInPixels] never returns
 * below it), so it scales deterministically without depending on headless HTML
 * layout precision. (Width is pinned to the fold's minimum when the editor
 * component is unrealized in the fixture, so height is the reliable signal.)
 */
class MarkdownZoomScalingTest : BasePlatformTestCase() {

    fun testWholeLineBlockHeightScalesWithEditorZoom() {
        assertTrue(
            "markdown-in-comments rendering must be enabled for this test",
            CajetaSettings.instance.renderMarkdownInComments,
        )

        // Two contiguous whole-line comments → one collapsed CustomFoldRegion
        // rendering the markdown in place.
        myFixture.configureByText(
            "Demo.cajeta",
            "// # Heading\n// body text\nint32 x = 1;\n",
        )
        val editor = myFixture.editor as EditorEx

        // The plugin's startup activity installs markdown rendering on editor
        // creation via an invokeLater; pump the queue to let it run.
        PlatformTestUtil.dispatchAllInvocationEventsInIdeEventQueue()

        val region = MarkdownFoldEditorListener.wholeLineBlocksFor(editor)
            .single().customRegion
        assertNotNull("a whole-line block should be installed and collapsed", region)
        val heightBefore = region!!.heightInPixels
        assertTrue("collapsed block should have a positive height", heightBefore > 0)

        // Zoom in: Ctrl+wheel drives exactly this setFontSize path.
        editor.setFontSize(editor.colorsScheme.editorFontSize + 8)
        PlatformTestUtil.dispatchAllInvocationEventsInIdeEventQueue()

        assertTrue(
            "fold-region height should grow with editor zoom " +
                "(was $heightBefore, now ${region.heightInPixels})",
            region.heightInPixels > heightBefore,
        )
    }

    /**
     * Zoom sets a *fractional* font size. Reading the deprecated Int
     * `editorFontSize` truncated it, so a sub-point zoom step produced an
     * identical render — and, because the cache keys compared equal, no
     * re-measure at all. The palette must carry the fraction through to the CSS.
     */
    fun testPaletteCarriesFractionalZoomIntoCss() {
        myFixture.configureByText("Demo.cajeta", "// # Heading\nint32 x = 1;\n")
        val editor = myFixture.editor as EditorEx
        PlatformTestUtil.dispatchAllInvocationEventsInIdeEventQueue()

        editor.colorsScheme.setEditorFontSize(13.5f)

        val palette = EditorMarkdownPalette.forEditor(editor, withBackground = false)
        assertEquals(
            "palette must read the fractional (2D) editor font size, not the truncated Int",
            13.5f, palette.fontSizePt, 0.01f,
        )
        assertTrue(
            "the fraction must survive into the stylesheet",
            MarkdownHtmlTheme.wrap("<p>x</p>", palette).contains("13.5pt"),
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
