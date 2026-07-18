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
import dev.cajeta.idea.settings.CajetaSettings
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * On project open, extract + mount read-only source roots in the BACKGROUND
 * (the extraction subprocesses are slow and must not run on the roots
 * provider's read action), publish them to [CajetaMountService], then fire a
 * roots change so the platform re-queries [CajetaXrefLibraryRootsProvider] and
 * indexes them. After that, Ctrl-click into stdlib AND dependency types
 * resolves (ide-symbol-index §8.3).
 *
 * Two sources:
 *  - the compiler's stdlib (`cajeta stdlib extract`, keyed on compiler identity);
 *  - every resolved dependency `.cja` under
 *    `<projectBase>/.cajeta/cache/artifacts/` — the content-hash path the
 *    resolver hands the compiler as `--classpath`. Each embeds its full
 *    `.cajeta` source (ClassSource entries), extracted by `cajeta archive
 *    extract` and keyed on the archive's content hash, so it is a
 *    once-per-version cost.
 *
 * Idempotent and cheap on a warm cache — mounts just re-register the existing
 * extracted dirs.
 */
class CajetaSourceMountStartup : ProjectActivity {

    private val log = Logger.getInstance(CajetaSourceMountStartup::class.java)

    override suspend fun execute(project: Project) {
        val roots = mutableListOf<VirtualFile>()
        val lfs = LocalFileSystem.getInstance()
        val compilerPath = CajetaSettings.instance.compilerPath

        withContext(Dispatchers.IO) {
            // Stdlib source.
            try {
                CajetaSourceMountGlue.ensureStdlibMounted()?.let { path ->
                    lfs.refreshAndFindFileByNioFile(path)?.let { roots.add(it) }
                }
            } catch (t: Throwable) {
                log.warn("stdlib source mount failed", t)
            }

            // Dependency .cja sources (§8.3.1).
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
        }

        if (roots.isEmpty()) return
        CajetaMountService.getInstance(project).setRoots(roots)

        // Re-query the roots provider + index the new source roots.
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
