package dev.cajeta.idea.buildtool

import dev.cajeta.idea.debugger.Json

/**
 * Parses the `cajeta tasks --json` document (spec §3) into a [TaskModel].
 * Tolerant by contract (§3.1.3): unknown fields are ignored (forward-compat),
 * missing optionals fall to defaults, a malformed entry is skipped, and
 * malformed top-level input returns a typed [Result.Failure] instead of
 * throwing — nothing here may propagate an exception into the EDT. Plain JVM
 * (reuses the bundled [Json]); no `com.intellij.*`.
 */
object TaskDiscoveryParser {

    sealed interface Result {
        data class Success(val model: TaskModel) : Result
        data class Failure(val reason: String) : Result
    }

    fun parse(jsonText: String): Result {
        val root = try {
            Json.parse(jsonText)
        } catch (e: Exception) {
            return Result.Failure("invalid JSON: ${e.message}")
        }
        if (root !is Json.Obj) return Result.Failure("expected a JSON object at top level")

        val manifest = root.opt("manifest")?.strOrNull() ?: ""
        val tasks = root.opt("tasks").asArray().mapNotNull { parseTask(it) }
        val builtins = root.opt("builtins").asArray().mapNotNull { parseBuiltin(it) }
        return Result.Success(TaskModel(manifest, tasks, builtins))
    }

    private fun parseTask(j: Json): CajetaTask? {
        val o = j as? Json.Obj ?: return null
        val name = o.opt("name")?.strOrNull() ?: return null   // a task without a name is unusable
        return CajetaTask(
            name = name,
            description = o.opt("description")?.strOrNull(),
            dependsOn = o.opt("dependsOn").asArray().mapNotNull { it.strOrNull() },
            params = o.opt("params").asArray().mapNotNull { parseParam(it) },
            runnable = (o.opt("runnable") as? Json.Bool)?.value ?: false,
            artifact = o.opt("artifact")?.strOrNull(),
        )
    }

    private fun parseParam(j: Json): TaskParam? {
        val o = j as? Json.Obj ?: return null
        val name = o.opt("name")?.strOrNull() ?: return null
        return TaskParam(
            name = name,
            type = o.opt("type")?.strOrNull() ?: "string",
            default = o.opt("default")?.strOrNull(),
            required = (o.opt("required") as? Json.Bool)?.value ?: false,
            doc = o.opt("doc")?.strOrNull(),
        )
    }

    private fun parseBuiltin(j: Json): BuiltinCommand? {
        val o = j as? Json.Obj ?: return null
        val name = o.opt("name")?.strOrNull() ?: return null
        return BuiltinCommand(name, o.opt("description")?.strOrNull())
    }

    private fun Json.strOrNull(): String? = (this as? Json.Str)?.value
    private fun Json?.asArray(): List<Json> = (this as? Json.Arr)?.items ?: emptyList()
}
