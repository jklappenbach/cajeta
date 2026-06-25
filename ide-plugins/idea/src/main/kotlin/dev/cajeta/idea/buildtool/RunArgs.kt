package dev.cajeta.idea.buildtool

/**
 * The pure Run-with-args model (spec §12): seeds typed task params from their
 * declared defaults, validates required ones, and composes a [TaskRunSpec] from
 * the dialog's selections (profile / flavor / `-P` properties / `-p` params).
 * Blank param values are omitted so an unset optional param isn't passed on the
 * command line. The Run-with-args dialog is a thin form over this. No
 * `com.intellij.*`.
 */
object RunArgs {

    /** Initial editable values for a task's params: the declared default, or "". */
    fun initialValues(task: CajetaTask): Map<String, String> =
        task.params.associate { it.name to (it.default ?: "") }

    /** Names of required params left blank in [values] (in declared order). */
    fun missingRequired(task: CajetaTask, values: Map<String, String>): List<String> =
        task.params.filter { it.required && values[it.name].isNullOrBlank() }.map { it.name }

    /** Compose the run spec; blank param values are dropped. */
    fun buildSpec(
        task: CajetaTask,
        manifestPath: String?,
        profile: String?,
        flavor: String?,
        properties: Map<String, String>,
        paramValues: Map<String, String>,
    ): TaskRunSpec = TaskRunSpec(
        task = task.name,
        manifestPath = manifestPath?.takeIf { it.isNotBlank() },
        profile = profile?.takeIf { it.isNotBlank() },
        flavor = flavor?.takeIf { it.isNotBlank() },
        properties = properties,
        params = paramValues.filterValues { it.isNotBlank() },
    )
}
