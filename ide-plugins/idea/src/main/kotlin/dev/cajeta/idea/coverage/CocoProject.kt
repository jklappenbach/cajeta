package dev.cajeta.idea.coverage

import com.intellij.openapi.project.Project
import dev.cajeta.idea.buildtool.CajetaManifest
import dev.cajeta.idea.debugger.Json
import java.io.File

/**
 * What a project's `cajeta.json` says about coverage.
 *
 * Run with Coverage cannot proceed without three answers, and all three live in
 * the manifest: is coco available at all, which task measures coverage, and
 * where do the artifacts land. Reading them up front is what lets the action
 * explain what is missing instead of failing obscurely (spec §4.3).
 *
 * Parsing is **total**, following [CajetaManifest]: absent, empty and malformed
 * input all report "not configured" with a reason. A half-written manifest is
 * the normal state while someone edits it and must never surface as a stack
 * trace from a Run action.
 */
data class CocoProject(
    /** True when `cajeta.coverage` appears in the manifest's plugin list. */
    val pluginDeclared: Boolean = false,
    /** Tasks that INSTRUMENT — a report-only task measures nothing. */
    val coverageTasks: List<String> = emptyList(),
    /** Artifact root, relative to the manifest directory. */
    val outDir: String = DEFAULT_OUT_DIR,
    /** Why coverage cannot be run, or null when it can. */
    val problem: String? = null,
) {
    val isConfigured: Boolean get() = problem == null

    /** The task Run with Coverage uses when the configuration names none. */
    val defaultTask: String? get() = coverageTasks.firstOrNull()

    companion object {
        const val DEFAULT_OUT_DIR: String = "build/coco"

        /**
         * Plugin ids that mean coco, newest first.
         *
         * `dev.cajeta.coverage` is what actually ships and what the resolver
         * publishes; `cajeta.coverage` is the first-party name BuildTool.md
         * still documents. Both occur in real manifests, and accepting only
         * one reports a correctly configured project as missing coco.
         *
         * The ACTION namespace is `cajeta.coverage.*` under either id — the
         * plugin declares those action names itself — so there is only one
         * spelling to match there.
         */
        private val PLUGIN_IDS = listOf("dev.cajeta.coverage", "cajeta.coverage")

        /** The id named in the "not configured" message. */
        private const val PLUGIN = "dev.cajeta.coverage"
        private const val INSTRUMENT = "cajeta.coverage.instrument"

        /** Read the project's manifest, or a not-configured result when absent. */
        fun of(project: Project): CocoProject {
            val path = CajetaManifest.path(project)
                ?: return CocoProject(problem = "this project has no cajeta.json")
            return try {
                parse(File(path).readText())
            } catch (e: Exception) {
                CocoProject(problem = "cajeta.json could not be read: ${e.message}")
            }
        }

        fun parse(text: String): CocoProject {
            val root = try {
                Json.parse(CajetaManifest.stripJsonComments(text))
            } catch (e: Exception) {
                null
            } ?: return CocoProject(problem = "cajeta.json is not valid JSON")

            val settings = root.opt("settings")
            // Top level is where the build tool actually reads the block
            // (Manifest.cpp's `plugins` key); `settings.plugins` is accepted
            // because manifests in the wild carry it and rejecting them would
            // report a working project as missing coco.
            val plugins = root.opt("plugins") ?: settings?.opt("plugins")
            val declaredEntry = (plugins as? Json.Obj)?.entries
                ?.let { e -> PLUGIN_IDS.firstNotNullOfOrNull { id -> e[id] } }
            val declared = declaredEntry != null

            val tasks = (root.opt("tasks") as? Json.Obj)?.entries.orEmpty()
            val instrumenting = tasks.filter { (_, task) ->
                actionsOf(task).any { (it.opt("action") as? Json.Str)?.value == INSTRUMENT }
            }

            val problem = when {
                !declared && instrumenting.isEmpty() ->
                    "this project does not use coverage: declare the \"$PLUGIN\" plugin " +
                        "in cajeta.json and bind \"$INSTRUMENT\" to a task"
                instrumenting.isEmpty() ->
                    "the \"$PLUGIN\" plugin is declared but no task invokes it: add a " +
                        "\"$INSTRUMENT\" action to the task that runs your suite"
                else -> null
            }

            return CocoProject(
                pluginDeclared = declared,
                coverageTasks = instrumenting.keys.toList(),
                outDir = outDirOf(declaredEntry, settings, root, instrumenting.values.firstOrNull()),
                problem = problem,
            )
        }

        private fun actionsOf(task: Json): List<Json> =
            (task.opt("actions") as? Json.Arr)?.items.orEmpty()

        /**
         * Per-action params override the plugin's config block — BuildTool.md:
         * config "applies unless the task overrides it".
         *
         * The config block lives at `plugins.<id>.config`, which is the only
         * shape the build tool implements. A `plugin-config` sibling block was
         * read here until the coco tour was written against a real manifest and
         * nothing matched: the key exists nowhere in `src/cajeta/buildtool`, so
         * every project that set `out` in the documented place was silently
         * read as `build/coco`. It stays as a last fallback and nothing more.
         */
        private fun outDirOf(declaredEntry: Json?, settings: Json?, root: Json, task: Json?): String {
            task?.let { t ->
                actionsOf(t)
                    .firstOrNull { (it.opt("action") as? Json.Str)?.value == INSTRUMENT }
                    ?.let { (it.opt("out") as? Json.Str)?.value }
                    ?.takeIf { it.isNotBlank() }
                    ?.let { return it }
            }
            outOf(declaredEntry?.opt("config"))?.let { return it }
            val legacy = (root.opt("plugin-config") ?: settings?.opt("plugin-config"))
            PLUGIN_IDS.forEach { id -> outOf(legacy?.opt(id))?.let { return it } }
            return DEFAULT_OUT_DIR
        }

        private fun outOf(config: Json?): String? =
            (config?.opt("out") as? Json.Str)?.value?.takeIf { it.isNotBlank() }
    }
}
