package dev.cajeta.idea.debugger

import com.intellij.openapi.options.SettingsEditor
import com.intellij.ui.components.JBCheckBox
import com.intellij.ui.components.JBTextField
import com.intellij.util.ui.FormBuilder
import dev.cajeta.idea.buildtool.CajetaManifest
import dev.cajeta.idea.buildtool.CajetaRoots
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
        // Prefill an empty source root from the manifest, else the conventional
        // layout, else the project base (spec §3). Written INTO the field, so
        // the developer sees and can change what will be used — never applied
        // invisibly at launch (spec 3.1.2). A value already set always wins.
        val base = configuration.project.basePath
        sourceRootField.text = if (base == null) configuration.sourceRoot else
            CajetaRoots.sourceRootFor(
                configuration.sourceRoot, base,
                CajetaManifest.buildSettings(configuration.project).sourceRoot)
        stopOnEntryCheck.isSelected = configuration.stopOnEntry
    }

    override fun applyEditorTo(configuration: CajetaRunConfiguration) {
        configuration.entryMethod = entryMethodField.text.trim()
        configuration.sourceRoot = sourceRootField.text.trim()
        configuration.stopOnEntry = stopOnEntryCheck.isSelected
    }

    override fun createEditor(): JComponent = panel
}
