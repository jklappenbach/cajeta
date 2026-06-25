package dev.cajeta.idea.markdown

import com.intellij.openapi.editor.colors.EditorColorsManager
import java.awt.Color

/**
 * Derives a [MarkdownHtmlTheme.Palette] from the active editor color scheme so
 * the rendered block tracks the editor's look (dark/light, font) rather than
 * looking like a foreign web widget. Shared by both rendering surfaces — Swing
 * (transparent body, painted over the fold tint) and JCEF (opaque body, since
 * Chromium has no editor showing through behind it).
 */
object EditorMarkdownPalette {

    fun current(withBackground: Boolean): MarkdownHtmlTheme.Palette {
        val scheme = EditorColorsManager.getInstance().globalScheme
        val fg = scheme.defaultForeground
        val bg = scheme.defaultBackground
        val dark = isDark(bg)
        return MarkdownHtmlTheme.Palette(
            foreground = fg.toHex(),
            muted = blend(fg, bg, 0.45).toHex(),
            accent = (if (dark) Color(0x4E, 0xA1, 0xFF) else Color(0x2F, 0x6F, 0xDB)).toHex(),
            codeBackground = blend(bg, fg, 0.08).toHex(),
            border = blend(bg, fg, 0.28).toHex(),
            fontName = scheme.editorFontName,
            fontSizePt = scheme.editorFontSize,
            background = if (withBackground) bg.toHex() else null,
        )
    }

    private fun isDark(c: Color): Boolean = (0.299 * c.red + 0.587 * c.green + 0.114 * c.blue) < 128

    /** Linear blend: [ratio] of [b] mixed into [a]. */
    private fun blend(a: Color, b: Color, ratio: Double): Color {
        val r = (a.red * (1 - ratio) + b.red * ratio).toInt()
        val g = (a.green * (1 - ratio) + b.green * ratio).toInt()
        val bl = (a.blue * (1 - ratio) + b.blue * ratio).toInt()
        return Color(r.coerceIn(0, 255), g.coerceIn(0, 255), bl.coerceIn(0, 255))
    }

    private fun Color.toHex(): String = "#%02x%02x%02x".format(red, green, blue)
}
