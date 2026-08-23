package dev.cajeta.idea.coverage

import com.intellij.openapi.fileEditor.OpenFileDescriptor
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.LocalFileSystem
import com.intellij.openapi.vfs.VirtualFile
import java.io.File

/**
 * Getting from a finding to the code (spec §6.1.4 — one action).
 *
 * coco records paths as the compiler saw them, which may be relative to a source
 * root, so the same resolution the coverage load uses is applied here. A finding
 * that cannot be resolved simply does not navigate; opening the wrong file would
 * be worse than opening none.
 */
object CocoNavigation {

    fun open(project: Project, cocoPath: String, line: Int): Boolean {
        val vf = resolve(project, cocoPath) ?: return false
        // Descriptor lines are 0-based; coco's are 1-based.
        OpenFileDescriptor(project, vf, (line - 1).coerceAtLeast(0), 0)
            .navigate(true)
        return true
    }

    fun resolve(project: Project, cocoPath: String): VirtualFile? {
        val fs = LocalFileSystem.getInstance()
        val direct = File(cocoPath)
        if (direct.isAbsolute) return fs.findFileByIoFile(direct)

        // The MANIFEST's root first, then the module model — [CocoSourceRoots],
        // the same order the coverage load uses.
        //
        // This used to consult `contentSourceRoots` alone, which is EMPTY for a
        // Cajeta project opened as a plain directory, and then fall back to
        // `basePath + cocoPath` — which is wrong whenever sources live under a
        // source root, i.e. always. `tour/coco/X.cajeta` became
        // `<project>/tour/coco/X.cajeta` instead of `<project>/src/...`, resolved
        // to nothing, and every Dead Code / Risk / Tests row silently refused to
        // navigate. Double-clicking a finding did nothing at all.
        //
        // Third site of one bug: the coverage loader and the coverage runner were
        // fixed for exactly this and this one was missed, because the fix went in
        // where the symptom was rather than everywhere the root cause was. The
        // grep worth doing was `contentSourceRoots`, not `gutter`.
        for (root in CocoSourceRoots.of(project)) {
            val candidate = File(root, cocoPath)
            if (candidate.isFile) {
                fs.findFileByIoFile(candidate)?.takeIf { it.isValid }?.let { return it }
            }
        }
        return project.basePath
            ?.let { File(it, cocoPath) }
            ?.takeIf { it.isFile }
            ?.let(fs::findFileByIoFile)
    }
}
