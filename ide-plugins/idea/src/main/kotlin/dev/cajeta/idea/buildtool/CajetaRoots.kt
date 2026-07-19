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
