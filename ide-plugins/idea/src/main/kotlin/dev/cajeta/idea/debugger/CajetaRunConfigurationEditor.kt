package dev.cajeta.idea.debugger

import com.intellij.execution.configuration.EnvironmentVariablesComponent
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.options.SettingsEditor
import com.intellij.openapi.ui.ComboBox
import com.intellij.ui.components.JBCheckBox
import com.intellij.ui.components.JBLabel
import com.intellij.ui.components.JBTextField
import com.intellij.util.Alarm
import com.intellij.util.ui.FormBuilder
import com.intellij.util.ui.UIUtil
import dev.cajeta.idea.buildtool.CajetaManifest
import dev.cajeta.idea.buildtool.CajetaRoots
import javax.swing.DefaultComboBoxModel
import javax.swing.JComponent

/** Settings panel for a Cajeta debug run configuration. */
class CajetaRunConfigurationEditor : SettingsEditor<CajetaRunConfiguration>() {

    // Editable: the index and manifest are suggestions, and an entry method
    // neither knows about must remain typeable (spec 1.4.1 / 2.1.1).
    private val entryMethodCombo = ComboBox<String>().apply { isEditable = true }
    private val entryMethodHint = JBLabel("").apply {
        font = UIUtil.getFont(UIUtil.FontSize.SMALL, font)
        foreground = UIUtil.getContextHelpForeground()
    }
    private val sourceRootField = JBTextField()
    private val stopOnEntryCheck = JBCheckBox("Stop on entry")
    // The platform's standard widget (spec 4.1.1): it supplies both the
    // name/value table and the "inherit system environment" toggle, so the
    // control reads and behaves the way it does in every other run
    // configuration.
    private val environmentComponent = EnvironmentVariablesComponent()

    private val panel: JComponent = FormBuilder.createFormBuilder()
        .addLabeledComponent("Entry method:", entryMethodCombo)
        .addComponentToRightColumn(entryMethodHint)
        .addLabeledComponent("Source root:", sourceRootField)
        .addComponent(environmentComponent)
        .addComponent(stopOnEntryCheck)
        .panel

    private var editorText: String
        get() = (entryMethodCombo.editor.item as? String).orEmpty()
        set(v) { entryMethodCombo.editor.item = v }

    override fun resetEditorFrom(configuration: CajetaRunConfiguration) {
        editorText = configuration.entryMethod

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
        environmentComponent.envs = configuration.envVars
        environmentComponent.isPassParentEnvs = configuration.inheritSystemEnv
        loadCandidates(configuration)
    }

    // Retry scheduling for the first-open race (2026-07-30): parented to this
    // editor, so pending retries die with the dialog.
    private val rescanAlarm = Alarm(Alarm.ThreadToUse.POOLED_THREAD, this)

    /**
     * Populate the dropdown off the EDT (3.2.5) — reading the index must never
     * freeze the settings dialog. The typed text is preserved across the swap,
     * so an in-flight edit is not clobbered when candidates arrive.
     *
     * First-open fix: the dialog used to query ONCE, racing project indexing —
     * a dumb-mode throw was even swallowed silently — so a fresh project
     * showed an empty dropdown until closed and reopened. Now an empty or
     * failed scan announces itself and retries while the index warms
     * ([EntryMethodCandidates.needsRetry]), and the open dialog fills in.
     */
    private fun loadCandidates(configuration: CajetaRunConfiguration, attempt: Int = 0) {
        val project = configuration.project
        ApplicationManager.getApplication().executeOnPooledThread {
            val result = runCatching { EntryMethodCandidates.forProject(project) }
                .getOrNull()
            ApplicationManager.getApplication().invokeLater {
                if (result != null) {
                    val keep = editorText
                    entryMethodCombo.model =
                        DefaultComboBoxModel(result.candidates.map { it.fqn }.toTypedArray())
                    editorText = keep.ifBlank {
                        // Nothing chosen yet: preselect the first DECLARED candidate,
                        // so a manifest project launches with no typing (spec 2.2.1).
                        result.candidates.firstOrNull { it.declared }?.fqn
                            ?: result.candidates.firstOrNull()?.fqn.orEmpty()
                    }
                }
                if (EntryMethodCandidates.needsRetry(result, attempt)) {
                    entryMethodHint.text = "Scanning for entry methods…"
                    rescanAlarm.cancelAllRequests()
                    rescanAlarm.addRequest(
                        { loadCandidates(configuration, attempt + 1) }, RESCAN_DELAY_MS)
                } else {
                    // "Index unavailable" and "index warm, found nothing" are
                    // different facts and must not read alike (spec 6.1.3).
                    entryMethodHint.text = when {
                        result == null -> "Entry-method scan failed — type pkg.Class::main."
                        else -> result.emptyMessage() ?: declaredHint(result)
                    }
                }
            }
        }
    }

    /** Marks which offers are the project's own declarations (spec 2.1.4). */
    private fun declaredHint(result: EntryMethodCandidates.Result): String {
        val declared = result.candidates.count { it.declared }
        val found = result.candidates.size - declared
        return buildString {
            if (declared > 0) append("$declared declared in cajeta.json")
            if (declared > 0 && found > 0) append(", ")
            if (found > 0) append("$found found in index")
        }
    }

    override fun applyEditorTo(configuration: CajetaRunConfiguration) {
        // Whatever was typed or picked persists in the normalized dotted form
        // (spec 2.1.5 / 2.2.6).
        configuration.entryMethod = EntryMethodCandidates.persistedValueFor(editorText)
        configuration.sourceRoot = sourceRootField.text.trim()
        configuration.stopOnEntry = stopOnEntryCheck.isSelected
        configuration.envVars = environmentComponent.envs
        configuration.inheritSystemEnv = environmentComponent.isPassParentEnvs
    }

    override fun createEditor(): JComponent = panel

    companion object {
        private const val RESCAN_DELAY_MS = 2000
    }
}
