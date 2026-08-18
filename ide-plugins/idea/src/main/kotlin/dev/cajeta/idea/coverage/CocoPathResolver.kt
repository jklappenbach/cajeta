package dev.cajeta.idea.coverage

import java.io.File

/**
 * Turns coco's `file` field into a path the IDE knows the file by.
 *
 * coco records the path as the compiler saw it — normally relative to a source
 * root (`probe/Cond.cajeta`), and explicitly not guaranteed to be absolute or
 * relative to any particular root. The IDE needs an absolute path, because
 * `SimpleCoverageAnnotator` matches `ProjectData`'s keys against real files.
 *
 * Resolution tries each candidate root in order and takes the first that names
 * an existing file. When nothing matches, the original string is returned
 * unchanged: an unresolvable entry then simply annotates nothing, rather than
 * being silently attached to whichever file happened to share its name.
 */
class CocoPathResolver(roots: List<File>) {

    private val roots: List<File> = roots.distinct()
    private val cache = HashMap<String, String>()

    fun resolve(cocoPath: String): String = cache.getOrPut(cocoPath) {
        val asGiven = File(cocoPath)
        if (asGiven.isAbsolute) {
            return@getOrPut normalize(asGiven)
        }
        for (root in roots) {
            val candidate = File(root, cocoPath)
            if (candidate.isFile) return@getOrPut normalize(candidate)
        }
        cocoPath
    }

    /** Separators are normalized so the annotator's own normalization matches. */
    private fun normalize(f: File): String = f.absolutePath.replace(File.separatorChar, '/')

    companion object {
        /**
         * Roots worth trying for a profile at `<base>/run/coco.profile`, most
         * specific first: the project's own source roots, then the coco output
         * base and its parent — which is what makes a self-contained artifact
         * directory (the conformance fixture's shape) resolve on its own.
         */
        fun forProfile(profile: File, sourceRoots: List<File>): CocoPathResolver {
            val dir = profile.absoluteFile.parentFile
            val base = dir?.parentFile
            return CocoPathResolver(
                sourceRoots + listOfNotNull(dir, base, base?.parentFile)
            )
        }
    }
}
