package dev.cajeta.idea.xref

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.project.Project
import com.intellij.openapi.startup.ProjectActivity
import com.intellij.openapi.vfs.VirtualFileManager
import com.intellij.openapi.vfs.newvfs.BulkFileListener
import com.intellij.openapi.vfs.newvfs.events.VFileEvent
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * On project open, extract + mount read-only source roots in the BACKGROUND
 * (the extraction subprocesses are slow and must not run on the roots
 * provider's read action) — stdlib plus resolved dependency `.cja`s; the
 * heavy lifting lives in [CajetaSourceMounts]. After that, Ctrl-click into
 * stdlib AND dependency types resolves (ide-symbol-index §8.3).
 *
 * Also subscribes to VFS changes for the life of the project: when a resolved
 * archive LANDS in `<base>/.cajeta/cache/artifacts/` — a fresh consumer
 * project's first build finishing — the mount re-runs, so dependency
 * navigation works without restarting the IDE (§8.3 first-open fix,
 * 2026-07-30). Debounced: one remount per burst of archive writes.
 */
class CajetaSourceMountStartup : ProjectActivity {

    override suspend fun execute(project: Project) {
        subscribeToArtifactDrops(project)
        withContext(Dispatchers.IO) {
            CajetaSourceMounts.mountAll(project)
        }
    }

    private fun subscribeToArtifactDrops(project: Project) {
        val remountPending = AtomicBoolean(false)
        project.messageBus.connect().subscribe(
            VirtualFileManager.VFS_CHANGES,
            object : BulkFileListener {
                override fun after(events: List<VFileEvent>) {
                    val base = project.basePath ?: return
                    if (events.none {
                            CajetaArtifactsWatch.isArtifactArchivePath(it.path, base)
                        }
                    ) return
                    if (!remountPending.compareAndSet(false, true)) return
                    com.intellij.util.concurrency.AppExecutorUtil
                        .getAppScheduledExecutorService().schedule({
                            remountPending.set(false)
                            if (!project.isDisposed) {
                                ApplicationManager.getApplication().executeOnPooledThread {
                                    CajetaSourceMounts.mountAll(project)
                                }
                            }
                        }, 2, TimeUnit.SECONDS)
                }
            },
        )
    }
}
