package dev.cajeta.idea.debugger

import com.intellij.openapi.options.SettingsEditor
import com.intellij.ui.components.JBCheckBox
import com.intellij.ui.components.JBTextField
import com.intellij.util.ui.FormBuilder
import javax.swing.JComponent

/** Settings panel for a Cajeta debug run configuration. */
class CajetaRunConfigurationEditor : SettingsEditor<CajetaRunConfiguration>() {

    private val entryMethodField = JBTextField()
    private val sourceRootField = JBTextField()
    private val stopOnEntryCheck = JBCheckBox("Stop on entry")

    private val panel: JComponent = FormBuilder.createFormBuilder()
        .addLabeledComponent("Entry method (e.g. demo.Calc.main):", entryMethodField)
        .addLabeledComponent("Source root:", sourceRootField)
        .addComponent(stopOnEntryCheck)
        .panel

    override fun resetEditorFrom(configuration: CajetaRunConfiguration) {
        entryMethodField.text = configuration.entryMethod
        sourceRootField.text = configuration.sourceRoot
        stopOnEntryCheck.isSelected = configuration.stopOnEntry
    }

    override fun applyEditorTo(configuration: CajetaRunConfiguration) {
        configuration.entryMethod = entryMethodField.text.trim()
        configuration.sourceRoot = sourceRootField.text.trim()
        configuration.stopOnEntry = stopOnEntryCheck.isSelected
    }

    override fun createEditor(): JComponent = panel
}
