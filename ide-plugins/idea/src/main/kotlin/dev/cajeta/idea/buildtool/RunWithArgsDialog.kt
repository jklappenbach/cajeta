package dev.cajeta.idea.buildtool

import com.intellij.openapi.project.Project
import com.intellij.openapi.ui.DialogWrapper
import com.intellij.openapi.ui.ValidationInfo
import com.intellij.ui.components.JBTextField
import com.intellij.util.ui.FormBuilder
import javax.swing.JComponent

/**
 * The Run-with-args dialog (spec §12.2): renders a task's typed params
 * (name/default/required/doc) plus profile, flavor, and `-P` property overrides,
 * and produces a [TaskRunSpec] via the pure [RunArgs] model. A thin form — all
 * seeding/validation/composition is in [RunArgs]; this only collects input.
 */
class RunWithArgsDialog(
    project: Project,
    private val task: CajetaTask,
    private val manifestPath: String?,
    defaultProfile: String,
    defaultFlavor: String,
) : DialogWrapper(project) {

    private val profileField = JBTextField(defaultProfile, 20)
    private val flavorField = JBTextField(defaultFlavor, 20)
    private val propertiesField = JBTextField(24)
    private val paramFields: Map<String, JBTextField> =
        RunArgs.initialValues(task).mapValues { (_, v) -> JBTextField(v, 20) }

    init {
        title = "Run ${task.name} with Arguments"
        init()
    }

    override fun createCenterPanel(): JComponent {
        val builder = FormBuilder.createFormBuilder()
            .addLabeledComponent("Profile:", profileField)
            .addLabeledComponent("Flavor:", flavorField)
            .addLabeledComponent("Properties (-P, comma-sep key=value):", propertiesField)
        for (p in task.params) {
            val label = buildString {
                append(p.name)
                if (p.required) append(" *")
                append(":")
            }
            val field = paramFields.getValue(p.name).apply { toolTipText = p.doc }
            builder.addLabeledComponent(label, field)
        }
        return builder.panel
    }

    override fun doValidate(): ValidationInfo? {
        val missing = RunArgs.missingRequired(task, currentParamValues())
        if (missing.isNotEmpty()) {
            val first = paramFields[missing.first()]
            return ValidationInfo("Required: ${missing.joinToString(", ")}", first)
        }
        return null
    }

    private fun currentParamValues(): Map<String, String> =
        paramFields.mapValues { (_, f) -> f.text.trim() }

    /** The composed run spec — valid only after OK (doValidate gates it). */
    fun result(): TaskRunSpec = RunArgs.buildSpec(
        task = task,
        manifestPath = manifestPath,
        profile = profileField.text.trim(),
        flavor = flavorField.text.trim(),
        properties = parseProperties(propertiesField.text),
        paramValues = currentParamValues(),
    )

    /** `-P` overrides typed as comma-separated `key=value`. */
    private fun parseProperties(text: String): Map<String, String> {
        val out = LinkedHashMap<String, String>()
        for (piece in text.split(',')) {
            val t = piece.trim()
            if (t.isEmpty()) continue
            val eq = t.indexOf('=')
            if (eq > 0) out[t.substring(0, eq).trim()] = t.substring(eq + 1).trim()
        }
        return out
    }
}
