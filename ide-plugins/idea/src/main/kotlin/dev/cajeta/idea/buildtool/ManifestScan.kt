package dev.cajeta.idea.buildtool

import java.io.File

/**
 * Finds a project's `cajeta.json` manifests. The root manifest is only one of
 * them: a repo routinely holds sub-projects (`samples/tour`, `tools/mcp`), and
 * a project opened at the repo root — or at a directory whose own manifest is
 * a library with no entry point — still has declared entry methods nearby.
 * Julian hit exactly this on cajeta-logging 2026-07-30.
 *
 * Bounded and cheap: a depth budget, and build output / dot directories are
 * never walked.
 */
object ManifestScan {

    const val FILENAME = "cajeta.json"

    private val SKIP = setOf("build", "node_modules", "target", "out", "dist")

    /** Directories worth descending into — no dot dirs, no build output. */
    fun shouldDescend(dirName: String): Boolean =
        !dirName.startsWith(".") && dirName !in SKIP

    /**
     * Every manifest at or under [base], nearest first, within [maxDepth]
     * directory levels. Total: an unreadable or missing tree yields empty.
     */
    fun findManifests(base: File, maxDepth: Int = 3): List<File> {
        if (!base.isDirectory) return emptyList()
        val out = ArrayList<File>()
        fun walk(dir: File, depth: Int) {
            File(dir, FILENAME).takeIf { it.isFile }?.let { out.add(it) }
            if (depth >= maxDepth) return
            val children = dir.listFiles() ?: return
            for (child in children.sortedBy { it.name }) {
                if (child.isDirectory && shouldDescend(child.name)) walk(child, depth + 1)
            }
        }
        walk(base, 0)
        return out
    }
}
