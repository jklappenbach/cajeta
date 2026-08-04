package dev.cajeta.idea.buildtool

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.diagnostic.Logger
import com.intellij.openapi.options.SettingsEditor
import com.intellij.openapi.ui.ComboBox
import dev.cajeta.idea.settings.CajetaSettings
import java.io.File
import com.intellij.ui.components.JBTextArea
import com.intellij.ui.components.JBTextField
import com.intellij.util.ui.FormBuilder
import javax.swing.JComponent

/** Settings panel for a Cajeta task run configuration (spec §6, §12). */
class CajetaTaskRunConfigurationEditor : SettingsEditor<CajetaTaskRunConfiguration>() {

    private val taskField = JBTextField()
    private val manifestField = JBTextField()
    // EDITABLE combo (spec: discovery suggests, never constrains). The list
    // is filled in the background from the compiler's own answer; until it
    // arrives — or if the query fails — this behaves exactly like the text
    // field it replaced, so nothing about it can block a build.
    private val profileField = ComboBox<String>().apply {
        isEditable = true
        addItem(CajetaProfileCandidates.DEFAULT_PROFILE)
    }
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
        setProfileText(configuration.profile)
        loadProfilesInBackground(configuration)
        flavorField.text = configuration.flavor
        propertiesArea.text = configuration.propertiesText
        paramsArea.text = configuration.paramsText
    }

    override fun applyEditorTo(configuration: CajetaTaskRunConfiguration) {
        configuration.task = taskField.text.trim()
        configuration.manifestPath = manifestField.text.trim()
        configuration.profile = profileText().trim()
        configuration.flavor = flavorField.text.trim()
        configuration.propertiesText = propertiesArea.text.trim()
        configuration.paramsText = paramsArea.text.trim()
    }

    override fun createEditor(): JComponent = panel

    private fun profileText(): String =
        (profileField.editor?.item ?: profileField.selectedItem)?.toString() ?: ""

    private fun setProfileText(value: String) {
        profileField.editor?.item = value
        profileField.selectedItem = value
    }

    /**
     * Ask the compiler what profiles this project declares, off the EDT.
     *
     * Everything here degrades to the previous behaviour: a missing compiler,
     * a missing source root, a non-zero exit or unreadable output all leave
     * the field exactly as editable as it was. The developer's own text is
     * never replaced by what discovery found.
     */
    private fun loadProfilesInBackground(configuration: CajetaTaskRunConfiguration) {
        val compiler = CajetaSettings.instance.buildToolPath
        val root = sourceRootFor(configuration)
        if (compiler.isBlank() || root == null) return
        ApplicationManager.getApplication().executeOnPooledThread {
            val result = try {
                val proc = ProcessBuilder(CajetaProfileCandidates.argvFor(compiler, root))
                    .redirectErrorStream(false)
                    .start()
                val out = proc.inputStream.bufferedReader().readText()
                proc.waitFor()
                CajetaProfileCandidates.parse(out)
            } catch (t: Throwable) {
                LOG.debug("profile discovery failed", t)
                CajetaProfileCandidates.Result(emptyList(), queried = false,
                    error = t.message)
            }
            ApplicationManager.getApplication().invokeLater {
                val typed = profileText()
                profileField.removeAllItems()
                for (p in result.offered()) profileField.addItem(p)
                // Whatever the developer had stays put, even if discovery
                // never saw it — a profile the scan missed must not vanish
                // from under them.
                setProfileText(typed)
                profileField.toolTipText = result.emptyMessage()
            }
        }
    }

    /** The source root to ask about: the manifest's directory, since that is
     *  what the task runs against. */
    private fun sourceRootFor(configuration: CajetaTaskRunConfiguration): String? {
        val manifest = configuration.manifestPath.ifBlank { null } ?: return null
        val dir = File(manifest).parentFile ?: return null
        val src = File(dir, "src")
        return if (src.isDirectory) src.absolutePath else dir.absolutePath
    }

    private companion object {
        val LOG = Logger.getInstance(CajetaTaskRunConfigurationEditor::class.java)
    }
}
