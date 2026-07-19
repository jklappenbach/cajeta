package dev.cajeta.idea.buildtool

import com.intellij.openapi.project.Project
import dev.cajeta.idea.debugger.Json
import java.io.File

/** Locates the project's `cajeta.json` manifest (spec §2.2: the tool window is
 *  only available when one exists), and reads its `settings.build` block. */
object CajetaManifest {

    /**
     * The `settings.build` values the run configuration defaults from
     * (run-config-ergonomics §2, §3). Entry methods are NORMALIZED on read, so
     * everything downstream sees the dotted form regardless of how the manifest
     * spelled it.
     *
     * [binaries] maps binary name → entry method; a multi-binary project
     * declares one per executable.
     */
    data class BuildSettings(
        val entryMethod: String? = null,
        val sourceRoot: String? = null,
        val binaries: Map<String, String> = emptyMap(),
    )

    fun path(project: Project): String? {
        val base = project.basePath ?: return null
        val f = File(base, "cajeta.json")
        return if (f.isFile) f.path else null
    }

    fun exists(project: Project): Boolean = path(project) != null

    /**
     * The one place `Class::method` becomes `Class.method` (spec 1.4.5). The
     * manifest writes `::`, the DAP wants `.`, and a second copy of this rule is
     * how the two spellings drift apart. Idempotent and total: a name with no
     * `::` is returned verbatim.
     */
    fun normalizeEntryMethod(raw: String): String = raw.replace("::", ".")

    /** [parseBuildSettings] for the project's manifest, or empty when absent. */
    fun buildSettings(project: Project): BuildSettings {
        val p = path(project) ?: return BuildSettings()
        return try {
            parseBuildSettings(File(p).readText())
        } catch (e: Exception) {
            BuildSettings()
        }
    }

    /**
     * Read `settings.build` from manifest text. Key spellings match the
     * compiler's Manifest.cpp: `entry-method`, `source-root`, and a `binaries`
     * OBJECT keyed by binary name.
     *
     * Total by contract — absent, empty, and malformed input all yield empty
     * values rather than throwing. A half-written `cajeta.json` is the normal
     * state while someone edits it, and it must never break the editor that
     * reads it (plan 1.1.3 / 1.1.4).
     */
    fun parseBuildSettings(text: String): BuildSettings {
        val build = try {
            Json.parse(text).opt("settings")?.opt("build")
        } catch (e: Exception) {
            null
        } ?: return BuildSettings()

        val entry = (build.opt("entry-method") as? Json.Str)?.value
        val root = (build.opt("source-root") as? Json.Str)?.value

        val binaries = LinkedHashMap<String, String>()
        (build.opt("binaries") as? Json.Obj)?.entries?.forEach { (name, spec) ->
            (spec.opt("entry-method") as? Json.Str)?.value?.let {
                binaries[name] = normalizeEntryMethod(it)
            }
        }

        return BuildSettings(
            entryMethod = entry?.let(::normalizeEntryMethod),
            sourceRoot = root,
            binaries = binaries,
        )
    }
}
