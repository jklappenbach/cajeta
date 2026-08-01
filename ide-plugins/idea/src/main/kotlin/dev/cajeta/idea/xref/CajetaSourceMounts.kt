package dev.cajeta.idea.xref

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.application.WriteAction
import com.intellij.openapi.diagnostic.Logger
import com.intellij.openapi.project.Project
import com.intellij.openapi.project.RootsChangeRescanningInfo
import com.intellij.openapi.roots.ex.ProjectRootManagerEx
import com.intellij.openapi.util.EmptyRunnable
import com.intellij.openapi.vfs.LocalFileSystem
import com.intellij.openapi.vfs.VirtualFile
import dev.cajeta.idea.settings.CajetaSettings
import java.io.File

/**
 * Computes and publishes the project's mounted read-only source roots —
 * stdlib plus every resolved dependency `.cja` under
 * `<base>/.cajeta/cache/artifacts/` — and fires a roots change when the set
 * actually changed. Shared by the project-open activity, the artifacts VFS
 * watch (a fresh consumer project's FIRST build resolves its dependencies
 * AFTER the startup mount already ran — Julian hit this live 2026-07-30:
 * "Logger is unknown", imports unclickable until an IDE restart), and the
 * xref rebuild action (so the manual "fix everything" action really does).
 */
object CajetaSourceMounts {

    private val log = Logger.getInstance(CajetaSourceMounts::class.java)

    /**
     * Extract + mount everything currently on disk; slow (subprocesses), call
     * off the EDT. Publishes to [CajetaMountService] and schedules the rescan
     * only when the root set changed, so repeated calls on a warm cache are
     * cheap and quiet. Returns whether the roots changed.
     */
    fun mountAll(project: Project): Boolean {
        val roots = mutableListOf<VirtualFile>()
        val lfs = LocalFileSystem.getInstance()
        val compilerPath = CajetaSettings.instance.compilerPath

        try {
            CajetaSourceMountGlue.ensureStdlibMounted()?.let { path ->
                lfs.refreshAndFindFileByNioFile(path)?.let { roots.add(it) }
            }
        } catch (t: Throwable) {
            log.warn("stdlib source mount failed", t)
        }

        try {
            if (compilerPath.isNotBlank() && File(compilerPath).canExecute()) {
                val cacheRoot = CajetaSourceMountGlue.defaultCacheRoot()
                val extractor = CajetaSourceMountGlue.archiveExtractor(compilerPath)
                for (cja in CajetaSourceMountGlue.dependencyArchives(project.basePath)) {
                    val mounted = CajetaMountedSources.mountArchive(
                        cja, cacheRoot, extractor) ?: continue
                    lfs.refreshAndFindFileByNioFile(mounted)?.let { roots.add(it) }
                }
            }
        } catch (t: Throwable) {
            log.warn("dependency source mount failed", t)
        }

        // A degraded pass (no compiler configured, extraction failures) must
        // never CLEAR previously mounted roots — keep serving the old set.
        if (roots.isEmpty()) return false
        val service = CajetaMountService.getInstance(project)
        val before = service.getRoots().map { it.path }.toSet()
        val after = roots.map { it.path }.toSet()
        if (after == before) return false
        service.setRoots(roots)

        ApplicationManager.getApplication().invokeLater {
            if (project.isDisposed) return@invokeLater
            WriteAction.run<RuntimeException> {
                ProjectRootManagerEx.getInstanceEx(project).makeRootsChange(
                    EmptyRunnable.getInstance(),
                    RootsChangeRescanningInfo.TOTAL_RESCAN)
            }
        }
        return true
    }
}
