package dev.cajeta.idea.debugger

import com.intellij.execution.configuration.EnvironmentVariablesComponent
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.application.ModalityState
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

        // Paint the manifest's declared entry methods SYNCHRONOUSLY, here on
        // the EDT, before the dialog is shown (3.2.8). Everything published
        // from a background thread goes through invokeLater, which is DEFERRED
        // while a modal dialog is open — the Run/Debug Configurations dialog is
        // modal, so those updates only landed after it closed, which is why the
        // dropdown was empty on first open and populated on reopen (Julian,
        // 2026-07-30). One small file read; the wider scan stays async.
        val declared = runCatching {
            EntryMethodCandidates.declaredFromRootManifest(configuration.project)
        }.getOrDefault(emptyList())
        if (declared.isNotEmpty()) {
            publish(declared)
            entryMethodHint.text = declaredHintFor(declared.size, 0)
        }

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
            // Phase 1 — DECLARED candidates, from manifest file reads alone.
            // No index, no read action, so a manifest project shows its entry
            // method immediately even while indexing is still running. This
            // coupling is what left samples/tour empty (2026-07-30).
            if (attempt == 0) {
                val declared = runCatching { EntryMethodCandidates.declaredForProject(project) }
                    .getOrDefault(emptyList())
                if (declared.isNotEmpty()) {
                    onUi { publish(declared) }
                }
            }

            // Phase 2 — merge in what the index discovered.
            val result = runCatching { EntryMethodCandidates.forProject(project) }
                .getOrNull()
            onUi {
                if (result != null) publish(result.candidates)
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

    /**
     * Run [block] on the EDT with `ModalityState.any()`. The default modality
     * for an `invokeLater` issued from a pooled thread is NON_MODAL, and those
     * runnables DO NOT RUN while a modal dialog is open — this editor lives in
     * the modal Run/Debug Configurations dialog, so a default `invokeLater`
     * silently waits for the dialog to close (3.2.8). Only Swing state is
     * touched here, which is what `any()` permits.
     */
    private fun onUi(block: () -> Unit) =
        ApplicationManager.getApplication().invokeLater(block, ModalityState.any())

    /**
     * Swap the dropdown's offers on the EDT, preserving whatever is typed —
     * an in-flight edit must not be clobbered when candidates arrive. With the
     * field still empty, preselect the first DECLARED candidate so a manifest
     * project launches with no typing (spec 2.2.1).
     */
    private fun publish(candidates: List<EntryMethodCandidates.Candidate>) {
        val keep = editorText
        entryMethodCombo.model = DefaultComboBoxModel(candidates.map { it.fqn }.toTypedArray())
        editorText = keep.ifBlank {
            candidates.firstOrNull { it.declared }?.fqn
                ?: candidates.firstOrNull()?.fqn.orEmpty()
        }
    }

    /** Marks which offers are the project's own declarations (spec 2.1.4). */
    private fun declaredHint(result: EntryMethodCandidates.Result): String =
        declaredHintFor(result.candidates.count { it.declared },
                        result.candidates.count { !it.declared })

    private fun declaredHintFor(declared: Int, found: Int): String = buildString {
        if (declared > 0) append("$declared declared in cajeta.json")
        if (declared > 0 && found > 0) append(", ")
        if (found > 0) append("$found found in index")
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
