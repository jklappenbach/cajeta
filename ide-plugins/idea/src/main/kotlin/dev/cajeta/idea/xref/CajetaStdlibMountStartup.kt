package dev.cajeta.idea.xref

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.application.WriteAction
import com.intellij.openapi.diagnostic.Logger
import com.intellij.openapi.project.Project
import com.intellij.openapi.project.RootsChangeRescanningInfo
import com.intellij.openapi.roots.ex.ProjectRootManagerEx
import com.intellij.openapi.startup.ProjectActivity
import com.intellij.openapi.util.EmptyRunnable
import com.intellij.openapi.vfs.LocalFileSystem
import com.intellij.openapi.vfs.VirtualFile
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * On project open, extract + mount the configured compiler's stdlib source in
 * the BACKGROUND (the `cajeta stdlib extract` subprocess is slow and must not
 * run on the roots-provider's read action), publish the mounted root to
 * [CajetaMountService], then fire a roots change so the platform re-queries
 * [CajetaXrefLibraryRootsProvider] and indexes the new source root. After
 * that, Ctrl-click into stdlib types resolves (ide-symbol-index §8.3).
 *
 * Idempotent and cheap on a cache hit — the mount is keyed on the compiler
 * identity, so a warm cache just re-registers the existing dir.
 */
class CajetaStdlibMountStartup : ProjectActivity {

    private val log = Logger.getInstance(CajetaStdlibMountStartup::class.java)

    override suspend fun execute(project: Project) {
        val roots = mutableListOf<VirtualFile>()
        try {
            val stdlibPath = withContext(Dispatchers.IO) {
                CajetaSourceMountGlue.ensureStdlibMounted()
            }
            if (stdlibPath != null) {
                val vf = withContext(Dispatchers.IO) {
                    LocalFileSystem.getInstance()
                        .refreshAndFindFileByNioFile(stdlibPath)
                }
                if (vf != null) roots.add(vf)
            }
        } catch (t: Throwable) {
            log.warn("stdlib source mount failed", t)
        }

        if (roots.isEmpty()) return
        CajetaMountService.getInstance(project).setRoots(roots)

        // Re-query the roots provider + index the new source root. A total
        // rescan is the reliable notification that additional library roots
        // appeared; it runs once per session (cache-warm mounts are fast).
        withContext(Dispatchers.IO) {
            ApplicationManager.getApplication().invokeLater {
                if (project.isDisposed) return@invokeLater
                WriteAction.run<RuntimeException> {
                    ProjectRootManagerEx.getInstanceEx(project).makeRootsChange(
                        EmptyRunnable.getInstance(),
                        RootsChangeRescanningInfo.TOTAL_RESCAN)
                }
            }
        }
    }
}
