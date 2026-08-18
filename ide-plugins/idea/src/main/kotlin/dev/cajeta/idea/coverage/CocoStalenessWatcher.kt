package dev.cajeta.idea.coverage

import com.intellij.openapi.editor.Document
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.openapi.fileEditor.FileDocumentManagerListener
import com.intellij.openapi.project.ProjectManager
import java.util.concurrent.atomic.AtomicReference

/**
 * Notices when a save invalidates the loaded coverage, and offers the re-run
 * (spec §5.1, §5.3).
 *
 * Saving is the right trigger: freshness compares content on disk, so an unsaved
 * edit has not invalidated anything yet, and warning about it would be wrong as
 * often as right.
 *
 * Told **once per set of stale files**, not once per save. Repeating on every
 * keystroke-driven autosave is how a warning becomes something people learn to
 * dismiss without reading, which costs more than saying nothing.
 */
class CocoStalenessWatcher : FileDocumentManagerListener {

    private val announced = AtomicReference<Set<String>>(emptySet())

    override fun beforeDocumentSaving(document: Document) {
        val file = FileDocumentManager.getInstance().getFile(document) ?: return
        if (!file.name.endsWith(".cajeta")) return

        for (project in ProjectManager.getInstance().openProjects) {
            if (project.isDisposed) continue
            val stale = staleSetIfNewlyAnnounced(project) ?: continue
            val lastRun = CocoLastRun.getInstance(project)
            CocoStaleNotifier.notifyIfStale(
                project,
                rerun = if (lastRun.canRerun()) ({ lastRun.rerun() }) else null,
            )
            check(stale.isNotEmpty())
        }
    }

    /**
     * The stale set when it is worth announcing, or null when it is not — either
     * nothing is stale, or this exact set has already been reported.
     *
     * Separated from the notification so the once-only decision is testable
     * without raising UI.
     */
    fun staleSetIfNewlyAnnounced(project: com.intellij.openapi.project.Project): Set<String>? {
        val freshness = CocoFreshness.getInstance(project)
        if (freshness.origin == null) return null
        val stale = freshness.staleFiles().toSet()
        if (stale.isEmpty()) {
            announced.set(emptySet())
            return null
        }
        return if (announced.getAndSet(stale) == stale) null else stale
    }
}
