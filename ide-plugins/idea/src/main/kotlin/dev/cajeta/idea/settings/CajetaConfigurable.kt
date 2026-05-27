package dev.cajeta.idea.settings

import com.intellij.openapi.options.Configurable
import com.intellij.ui.components.JBCheckBox
import com.intellij.ui.components.JBLabel
import com.intellij.ui.components.JBTextField
import com.intellij.util.ui.FormBuilder
import javax.swing.JComponent
import javax.swing.JPanel

class CajetaConfigurable : Configurable {

    private var compilerPathField: JBTextField? = null
    private var renderMarkdownCheck: JBCheckBox? = null
    private var fixturesPathField: JBTextField? = null
    private var typingDelayField: JBTextField? = null
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

        panel = FormBuilder.createFormBuilder()
            .addLabeledComponent(JBLabel("cajetac binary:"), pathField, 1, false)
            .addComponent(renderCheck, 1)
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
            (typingDelayField?.text?.toIntOrNull() ?: s.testTypingDelayMs) != s.testTypingDelayMs
    }

    override fun apply() {
        val s = CajetaSettings.instance
        s.compilerPath = compilerPathField?.text?.trim().orEmpty()
        s.renderMarkdownInComments = renderMarkdownCheck?.isSelected ?: true
        s.testFixturesPath = fixturesPathField?.text?.trim().orEmpty()
        typingDelayField?.text?.toIntOrNull()?.let {
            s.testTypingDelayMs = it.coerceIn(1, 5000)
        }
    }

    override fun reset() {
        val s = CajetaSettings.instance
        compilerPathField?.text = s.compilerPath
        renderMarkdownCheck?.isSelected = s.renderMarkdownInComments
        fixturesPathField?.text = s.testFixturesPath
        typingDelayField?.text = s.testTypingDelayMs.toString()
    }

    override fun disposeUIResources() {
        compilerPathField = null
        renderMarkdownCheck = null
        fixturesPathField = null
        typingDelayField = null
        panel = null
    }
}
