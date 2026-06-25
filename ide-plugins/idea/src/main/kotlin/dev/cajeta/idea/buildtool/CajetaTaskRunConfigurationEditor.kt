package dev.cajeta.idea.buildtool

import com.intellij.openapi.options.SettingsEditor
import com.intellij.ui.components.JBTextArea
import com.intellij.ui.components.JBTextField
import com.intellij.util.ui.FormBuilder
import javax.swing.JComponent

/** Settings panel for a Cajeta task run configuration (spec §6, §12). */
class CajetaTaskRunConfigurationEditor : SettingsEditor<CajetaTaskRunConfiguration>() {

    private val taskField = JBTextField()
    private val manifestField = JBTextField()
    private val profileField = JBTextField()
    private val flavorField = JBTextField()
    private val propertiesArea = JBTextArea(3, 30)
    private val paramsArea = JBTextArea(3, 30)

    private val panel: JComponent = FormBuilder.createFormBuilder()
        .addLabeledComponent("Task:", taskField)
        .addLabeledComponent("Manifest (cajeta.json):", manifestField)
        .addLabeledComponent("Profile:", profileField)
        .addLabeledComponent("Flavor:", flavorField)
        .addLabeledComponent("Properties (-P, key=value per line):", propertiesArea)
        .addLabeledComponent("Params (-p, key=value per line):", paramsArea)
        .panel

    override fun resetEditorFrom(configuration: CajetaTaskRunConfiguration) {
        taskField.text = configuration.task
        manifestField.text = configuration.manifestPath
        profileField.text = configuration.profile
        flavorField.text = configuration.flavor
        propertiesArea.text = configuration.propertiesText
        paramsArea.text = configuration.paramsText
    }

    override fun applyEditorTo(configuration: CajetaTaskRunConfiguration) {
        configuration.task = taskField.text.trim()
        configuration.manifestPath = manifestField.text.trim()
        configuration.profile = profileField.text.trim()
        configuration.flavor = flavorField.text.trim()
        configuration.propertiesText = propertiesArea.text.trim()
        configuration.paramsText = paramsArea.text.trim()
    }

    override fun createEditor(): JComponent = panel
}
