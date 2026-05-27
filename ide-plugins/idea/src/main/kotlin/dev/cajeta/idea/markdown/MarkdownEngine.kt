package dev.cajeta.idea.markdown

/**
 * Pluggable markdown renderer. Default v0.1 implementation uses the
 * IntelliJ-bundled `org.jetbrains:markdown` library with the GFM
 * flavour. The interface exists so a different engine can be swapped
 * in without touching the folding builder, renderer, or caret
 * listener — see ide-plugins/idea/Plan.md Step 8.
 */
interface MarkdownEngine {
    /** Convert markdown source to HTML suitable for JBHtmlEditorKit. */
    fun renderToHtml(markdown: String): String

    /** Stable identifier for caching and settings UI. */
    val id: String

    /** Human-readable name shown in Settings | Languages & Frameworks | Cajeta. */
    val displayName: String
}
