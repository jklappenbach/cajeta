package dev.cajeta.idea.coverage

import com.intellij.openapi.fileEditor.OpenFileDescriptor
import com.intellij.openapi.project.Project
import com.intellij.openapi.roots.ProjectRootManager
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
        for (root in ProjectRootManager.getInstance(project).contentSourceRoots) {
            val candidate = root.findFileByRelativePath(cocoPath)
            if (candidate != null && candidate.isValid) return candidate
        }
        return project.basePath
            ?.let { File(it, cocoPath) }
            ?.takeIf { it.isFile }
            ?.let(fs::findFileByIoFile)
    }
}
