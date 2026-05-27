package dev.cajeta.idea.markdown.engines

import dev.cajeta.idea.markdown.MarkdownEngine
import org.intellij.markdown.flavours.gfm.GFMFlavourDescriptor
import org.intellij.markdown.html.HtmlGenerator
import org.intellij.markdown.parser.MarkdownParser

class JetBrainsMarkdownEngine : MarkdownEngine {
    override val id: String = "jetbrains-markdown"
    override val displayName: String = "JetBrains Markdown (CommonMark + GFM)"

    private val flavour = GFMFlavourDescriptor()
    private val parser = MarkdownParser(flavour)

    override fun renderToHtml(markdown: String): String {
        val tree = parser.buildMarkdownTreeFromString(markdown)
        return HtmlGenerator(markdown, tree, flavour).generateHtml()
    }
}
