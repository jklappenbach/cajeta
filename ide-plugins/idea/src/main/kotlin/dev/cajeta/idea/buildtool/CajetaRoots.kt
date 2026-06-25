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
