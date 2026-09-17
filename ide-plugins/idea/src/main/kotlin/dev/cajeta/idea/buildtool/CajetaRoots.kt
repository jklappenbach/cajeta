package dev.cajeta.idea.buildtool

import dev.cajeta.idea.debugger.Json
import java.io.File

/**
 * An immutable, deduped, stably-ordered set of linked `cajeta.json` root paths
 * (spec §10). Link/unlink return a new value; persistence and the tree are built
 * on top. Pure.
 */
data class LinkedRoots(val paths: List<String>) {

    fun link(path: String): LinkedRoots =
        if (path.isBlank() || path in paths) this else LinkedRoots((paths + path).sorted())

    fun unlink(path: String): LinkedRoots = LinkedRoots(paths.filterNot { it == path })

    fun contains(path: String): Boolean = path in paths
}

/**
 * Root-discovery helpers (spec §10): a workspace manifest's `workspace.members`
 * expand to each member's `cajeta.json`, so a workspace root shows its members as
 * child roots (§10.2.4). Reuses the bundled [Json]; tolerant — malformed or
 * non-workspace input yields no members rather than throwing.
 */
object CajetaRoots {

    /**
     * The conventional source root for a project base: `<base>/src/main/cajeta`
     * when that directory exists, else `<base>`.
     *
     * The single definition of that convention. The xref whole-root export and
     * the debug run configuration both resolve through here, so the index and
     * the run configuration always describe the SAME tree (spec 3.1.3) — two
     * copies of this rule would let them disagree about what the project is.
     */
    fun conventionalSourceRoot(basePath: String): String =
        File(basePath, "src/main/cajeta").takeIf { it.isDirectory }?.path ?: basePath

    /** Trees that hold copies of sources rather than sources: discovering a root
     *  inside one would export a stale duplicate of the real tree. */
    private val EXCLUDED_DIRS = setOf(
        "build", "tmp", ".cajeta", ".git", ".idea", "out", "target", "node_modules",
    )

    /** Enough of a file to carry its `package` declaration; the rest is not read. */
    private const val HEAD_BYTES = 8192

    /**
     * EVERY source root the project has (lint-own-archive-classpath spec §8.2),
     * stably ordered.
     *
     * [conventionalSourceRoot] answers "the one root" and is right for a run
     * configuration, which launches one entry point. It is wrong for the xref
     * export, which must VISIT every root: a project whose tests live outside
     * `src/main/cajeta` had no index for them at all, so every import in a test
     * file was dead. Measured on cajeta-http — the shard named `HttpSerializer`
     * 587 times and `ServerTests` 0.
     *
     * Roots are DERIVED, not matched against conventional paths. There is no
     * convention to match: surveyed 2026-09-13, cajeta-http uses `test/src`
     * while cajeta-codec and cajeta-logging use `src/test/cajeta`, and no
     * manifest declares a test root — so a candidate list would silently give a
     * fourth layout no index, and an empty shard looks exactly like an unbuilt
     * project. Instead each file's declared package is stripped from the tail of
     * its path, which is [dev.cajeta.idea.lint.CajetacRunner.sourceRootOf] — the
     * SAME function lint uses to pick its root, called rather than copied, so
     * coverage and resolution cannot drift apart.
     */
    fun sourceRootsOf(basePath: String): List<String> {
        val base = File(basePath)
        if (!base.isDirectory) return emptyList()
        val roots = LinkedHashSet<String>()
        base.walkTopDown()
            .onEnter { it == base || it.name !in EXCLUDED_DIRS && !it.name.startsWith(".") }
            .filter { it.isFile && it.name.endsWith(".cajeta") }
            .forEach { f ->
                val head = runCatching {
                    f.inputStream().use { s -> String(s.readNBytes(HEAD_BYTES)) }
                }.getOrNull() ?: return@forEach
                roots.add(dev.cajeta.idea.lint.CajetacRunner.sourceRootOf(f.path, head))
            }
        return roots.sorted()
    }

    /**
     * The source root to prefill for a project (spec 3.1.1), in order:
     * [manifestSourceRoot] resolved against [basePath], then the convention
     * above. An absolute manifest value is used verbatim.
     */
    fun defaultSourceRoot(basePath: String, manifestSourceRoot: String?): String {
        val declared = manifestSourceRoot?.takeIf { it.isNotBlank() }
        if (declared != null) {
            val f = File(declared)
            return if (f.isAbsolute) f.path else File(basePath, declared).path
        }
        return conventionalSourceRoot(basePath)
    }

    /**
     * What the editor should show: a deliberately-set value always wins, and
     * only a blank one falls through to the default (spec 3.2.4). Defaults are
     * suggestions, never overrides (spec 1.4.1).
     */
    fun sourceRootFor(persisted: String, basePath: String,
                      manifestSourceRoot: String?): String =
        persisted.takeIf { it.isNotBlank() }
            ?: defaultSourceRoot(basePath, manifestSourceRoot)

    /** Absolute member-manifest paths for a workspace manifest rooted at
     *  [workspaceDir]; empty when the manifest has no `workspace.members`. */
    fun workspaceMembers(manifestText: String, workspaceDir: String): List<String> {
        val root = try {
            Json.parse(manifestText)
        } catch (_: Exception) {
            return emptyList()
        }
        val members = (root as? Json.Obj)?.opt("workspace")?.opt("members") as? Json.Arr
            ?: return emptyList()
        return members.items
            .mapNotNull { (it as? Json.Str)?.value }
            .filter { it.isNotBlank() }
            .map { File(File(workspaceDir, it), "cajeta.json").path }
    }
}
