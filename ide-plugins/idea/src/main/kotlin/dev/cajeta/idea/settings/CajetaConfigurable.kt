package dev.cajeta.idea.settings

import com.intellij.openapi.options.Configurable
import com.intellij.ui.components.JBCheckBox
import com.intellij.ui.components.JBLabel
import com.intellij.ui.components.JBTextArea
import com.intellij.ui.components.JBTextField
import com.intellij.util.ui.FormBuilder
import dev.cajeta.idea.debugger.MemoryFacetLegend
import javax.swing.JComponent
import javax.swing.JPanel

class CajetaConfigurable : Configurable {

    private var compilerPathField: JBTextField? = null
    private var renderMarkdownCheck: JBCheckBox? = null
    private var fixturesPathField: JBTextField? = null
    private var typingDelayField: JBTextField? = null
    // CP7-5: memory-facet visualization toggles (FR-7.3).
    private var facetsVariablesCheck: JBCheckBox? = null
    private var facetsGutterCheck: JBCheckBox? = null
    private var facetsInlineCheck: JBCheckBox? = null
    private var panel: JPanel? = null

    override fun getDisplayName(): String = "Cajeta"

    override fun createComponent(): JComponent {
        val settings = CajetaSettings.instance
        val pathField = JBTextField(settings.compilerPath, 40).also { compilerPathField = it }
        val renderCheck = JBCheckBox(
            "Render markdown in comments (Obsidian-style)",
            settings.renderMarkdownInComments,
        ).also { renderMarkdownCheck = it }
        val fixturesField = JBTextField(settings.testFixturesPath, 40).also { fixturesPathField = it }
        val delayField = JBTextField(settings.testTypingDelayMs.toString(), 6).also { typingDelayField = it }

        val varsCheck = JBCheckBox(
            "Show ownership/allocation/lifetime in the Variables view",
            settings.showFacetsInVariables,
        ).also { facetsVariablesCheck = it }
        val gutterCheck = JBCheckBox(
            "Show memory-facet icons in the editor gutter while debugging",
            settings.showFacetsInGutter,
        ).also { facetsGutterCheck = it }
        val inlineCheck = JBCheckBox(
            "Show inline memory-facet hints on the current line while debugging",
            settings.showFacetsInline,
        ).also { facetsInlineCheck = it }
        val legend = JBTextArea(MemoryFacetLegend.text()).apply {
            isEditable = false
            isOpaque = false
            lineWrap = false
        }

        panel = FormBuilder.createFormBuilder()
            .addLabeledComponent(JBLabel("cajetac binary:"), pathField, 1, false)
            .addComponent(renderCheck, 1)
            .addSeparator()
            .addComponent(JBLabel("Debugger — memory ownership visualization:"), 1)
            .addComponent(varsCheck, 1)
            .addComponent(gutterCheck, 1)
            .addComponent(inlineCheck, 1)
            .addComponent(JBLabel("Legend:"), 1)
            .addComponent(legend, 1)
            .addSeparator()
            .addLabeledComponent(JBLabel("Test fixtures path:"), fixturesField, 1, false)
            .addLabeledComponent(JBLabel("Typing-harness delay (ms):"), delayField, 1, false)
            .addComponentFillVertically(JPanel(), 0)
            .panel
        return panel!!
    }

    override fun isModified(): Boolean {
        val s = CajetaSettings.instance
        return (compilerPathField?.text ?: "") != s.compilerPath ||
            (renderMarkdownCheck?.isSelected ?: true) != s.renderMarkdownInComments ||
            (fixturesPathField?.text ?: "") != s.testFixturesPath ||
            (typingDelayField?.text?.toIntOrNull() ?: s.testTypingDelayMs) != s.testTypingDelayMs ||
            (facetsVariablesCheck?.isSelected ?: true) != s.showFacetsInVariables ||
            (facetsGutterCheck?.isSelected ?: true) != s.showFacetsInGutter ||
            (facetsInlineCheck?.isSelected ?: true) != s.showFacetsInline
    }

    override fun apply() {
        val s = CajetaSettings.instance
        s.compilerPath = compilerPathField?.text?.trim().orEmpty()
        s.renderMarkdownInComments = renderMarkdownCheck?.isSelected ?: true
        s.testFixturesPath = fixturesPathField?.text?.trim().orEmpty()
        typingDelayField?.text?.toIntOrNull()?.let {
            s.testTypingDelayMs = it.coerceIn(1, 5000)
        }
        s.showFacetsInVariables = facetsVariablesCheck?.isSelected ?: true
        s.showFacetsInGutter = facetsGutterCheck?.isSelected ?: true
        s.showFacetsInline = facetsInlineCheck?.isSelected ?: true
    }

    override fun reset() {
        val s = CajetaSettings.instance
        compilerPathField?.text = s.compilerPath
        renderMarkdownCheck?.isSelected = s.renderMarkdownInComments
        fixturesPathField?.text = s.testFixturesPath
        typingDelayField?.text = s.testTypingDelayMs.toString()
        facetsVariablesCheck?.isSelected = s.showFacetsInVariables
        facetsGutterCheck?.isSelected = s.showFacetsInGutter
        facetsInlineCheck?.isSelected = s.showFacetsInline
    }

    override fun disposeUIResources() {
        compilerPathField = null
        renderMarkdownCheck = null
        fixturesPathField = null
        typingDelayField = null
        facetsVariablesCheck = null
        facetsGutterCheck = null
        facetsInlineCheck = null
        panel = null
    }
}
