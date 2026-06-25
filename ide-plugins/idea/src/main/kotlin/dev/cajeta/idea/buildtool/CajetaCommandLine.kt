package dev.cajeta.idea.buildtool

/**
 * What to run: a task plus the per-run selections (spec §12). `properties` are
 * `-P key=value` overrides; `params` are typed task params (`-p key=value`).
 */
data class TaskRunSpec(
    val task: String,
    val manifestPath: String? = null,
    val profile: String? = null,
    val flavor: String? = null,
    val properties: Map<String, String> = emptyMap(),
    val params: Map<String, String> = emptyMap(),
)

/**
 * Builds the `cajeta` argv (excluding the executable) for task runs and for
 * task discovery (spec §12, §15.2). Pure and deterministic — properties/params
 * emit in sorted-key order — so the exact command line is unit-testable and the
 * Run-with-args dialog / run configs can preview it. Flags use the form the
 * build-tool binary parses: `--profile=`, `--flavor=`, `-P k=v`, `-p k=v`.
 */
object CajetaCommandLine {

    fun runArgv(spec: TaskRunSpec): List<String> {
        val out = mutableListOf<String>()
        out += spec.task
        spec.manifestPath?.takeIf { it.isNotBlank() }?.let { out += "--manifest=$it" }
        spec.profile?.takeIf { it.isNotBlank() }?.let { out += "--profile=$it" }
        spec.flavor?.takeIf { it.isNotBlank() }?.let { out += "--flavor=$it" }
        for ((k, v) in spec.properties.toSortedMap()) {
            out += "-P"; out += "$k=$v"
        }
        for ((k, v) in spec.params.toSortedMap()) {
            out += "-p"; out += "$k=$v"
        }
        return out
    }

    /** Argv for `cajeta tasks --json [--manifest=<path>]` (discovery). */
    fun discoveryArgv(manifestPath: String?): List<String> {
        val out = mutableListOf("tasks", "--json")
        manifestPath?.takeIf { it.isNotBlank() }?.let { out += "--manifest=$it" }
        return out
    }
}
