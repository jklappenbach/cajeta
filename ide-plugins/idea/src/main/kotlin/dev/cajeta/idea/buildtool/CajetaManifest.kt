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

    /**
     * `cajeta.json` is JSONC, not strict JSON: it allows `//` line comments,
     * `/* */` block comments, and trailing commas. This mirrors the compiler's
     * preprocessor in `src/cajeta/buildtool/JsonC.h`, which is the authority on
     * what the manifest format accepts.
     *
     * Length is PRESERVED — stripped spans become spaces rather than being
     * deleted — so any offset a downstream parse error reports still points at
     * the right place in the original file.
     *
     * String-aware by necessity: `"https://example.com"` holds a `//` that is
     * data, and a stripper that ignored that would silently eat the rest of
     * every line containing a URL.
     */
    fun stripJsonComments(source: String): String {
        val a = CharArray(source.length)

        // Pass 1 — comments become whitespace. String-aware: a `//` inside
        // "https://..." is data, and eating it would swallow the rest of the line.
        var i = 0
        var inString = false
        while (i < source.length) {
            val c = source[i]
            if (inString) {
                a[i] = c
                // Copy an escape pair wholesale: `\"` does not close the string.
                if (c == '\\' && i + 1 < source.length) { a[i + 1] = source[i + 1]; i += 2; continue }
                if (c == '"') inString = false
                i++
                continue
            }
            when {
                c == '"' -> { inString = true; a[i] = c; i++ }
                c == '/' && i + 1 < source.length && source[i + 1] == '/' ->
                    while (i < source.length && source[i] != '\n') { a[i] = ' '; i++ }
                c == '/' && i + 1 < source.length && source[i + 1] == '*' -> {
                    val end = source.indexOf("*/", i + 2)
                    val stop = if (end < 0) source.length else end + 2
                    while (i < stop) { a[i] = if (source[i] == '\n') '\n' else ' '; i++ }
                }
                else -> { a[i] = c; i++ }
            }
        }

        // Pass 2 — a comma is TRAILING only if the next significant character
        // closes the container. Deciding that by look-ahead, rather than by
        // tracking pending commas, is what keeps `["a", "b"]` intact: an
        // earlier version blanked that separator and produced invalid JSON.
        i = 0
        inString = false
        while (i < a.size) {
            val c = a[i]
            if (inString) {
                if (c == '\\') { i += 2; continue }
                if (c == '"') inString = false
                i++
                continue
            }
            when {
                c == '"' -> { inString = true; i++ }
                c == ',' -> {
                    var j = i + 1
                    while (j < a.size && a[j].isWhitespace()) j++
                    if (j < a.size && (a[j] == '}' || a[j] == ']')) a[i] = ' '
                    i++
                }
                else -> i++
            }
        }
        return String(a)
    }

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
            Json.parse(stripJsonComments(text)).opt("settings")?.opt("build")
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
