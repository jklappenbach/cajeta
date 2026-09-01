package dev.cajeta.idea.profiler

/**
 * How the timeline canvas sizes itself against its viewport.
 *
 * The canvas is a plain [javax.swing.JPanel] inside a scroll pane, and a plain
 * JPanel does not track its viewport. That left the timeline with a preferred
 * width fixed at 800: wider tool windows painted the lanes into 800 px and left
 * dead space beside them, and taller track lists were clipped rather than
 * scrolled, because nothing ever asked for a scrollbar.
 *
 * Both decisions are pure functions of two integers, so they are settled here
 * rather than inside an anonymous Swing object where nothing can reach them.
 */
object TimelineViewport {

    /**
     * The canvas is only as tall as its rows. Returning this as the preferred
     * height is what makes a vertical scrollbar appear at all — a viewport
     * cannot know that thirty tracks need more room than it has.
     */
    fun contentHeight(rows: Int, rowHeight: Int, rulerHeight: Int, pad: Int): Int =
        rows.coerceAtLeast(0) * rowHeight + rulerHeight + pad

    /**
     * Stretch to a viewport WIDER than the minimum, scroll when it is narrower.
     *
     * Both halves matter. Always stretching means a narrow tool window crushes
     * every lane into a few pixels, where a 30 s run and an 8 us kernel are the
     * same smear. Never stretching means a wide one paints into a fixed column
     * and wastes the rest.
     */
    fun tracksViewportWidth(viewportWidth: Int, minContentWidth: Int): Boolean =
        viewportWidth >= minContentWidth

    /**
     * Fill a viewport taller than the content, scroll when it is shorter.
     * Without the first half a short track list leaves the viewport's own
     * background showing under the rows, which reads as a rendering fault.
     */
    fun tracksViewportHeight(viewportHeight: Int, contentHeight: Int): Boolean =
        viewportHeight >= contentHeight
}
