package dev.cajeta.idea.xref

import com.intellij.openapi.components.Service
import com.intellij.openapi.project.Project
import dev.cajeta.idea.lint.XrefStream
import dev.cajeta.idea.lint.XrefStreamParser
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicReference

/**
 * The index freshness state machine (ide-symbol-index Unit 9, spec §7/§8):
 * `fresh | refreshing | stale | unavailable`, with an actionable reason.
 * Queryable so consumers degrade honestly — navigation keeps answering from
 * the last known export while REFRESHING; ide-features refuses to refactor on
 * STALE; UNAVAILABLE says WHY (no compiler, unknown schema major) instead of
 * failing silently. Locals always resolve regardless (§4.3 — PSI-only).
 */
@Service(Service.Level.PROJECT)
class CajetaXrefFreshness {

    enum class State { FRESH, REFRESHING, STALE, UNAVAILABLE }

    private data class Snapshot(val state: State, val reason: String?)

    private val snap = AtomicReference(Snapshot(State.STALE, "no export ingested yet"))

    /**
     * Notified when the snapshot actually CHANGES. The status widget is the
     * consumer: a StatusBarWidget renders only when the platform repaints it,
     * so a state machine that flips silently leaves the bar reading whatever it
     * last drew — reported live 2026-08-24 as "still reads stale after an index
     * update", against an index that had rebuilt fine.
     *
     * Only real changes fire. `updateFromLint` runs on every edit's lint
     * result, and repainting the status bar per keystroke to redraw the same
     * four characters is waste.
     */
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    fun addChangeListener(l: () -> Unit) { listeners += l }
    fun removeChangeListener(l: () -> Unit) { listeners.remove(l) }

    private fun publish(next: Snapshot) {
        if (snap.getAndSet(next) == next) return
        for (l in listeners) l()
    }

    val state: State get() = snap.get().state
    val reason: String? get() = snap.get().reason

    /** ide-features Unit 5's gate (9.1.6). */
    fun safeForRefactoring(): Boolean = state == State.FRESH

    fun refreshStarted() { publish(Snapshot(State.REFRESHING, null)) }
    fun refreshSucceeded() { publish(Snapshot(State.FRESH, null)) }
    fun refreshFailed(reason: String) { publish(Snapshot(State.STALE, reason)) }

    /**
     * Per-edit lint outcome → state. Called by the annotator's apply().
     *
     * [archive] reports whether the project's own build artifact is usable
     * (lint-own-archive-classpath Unit 4). It is published here, on the existing
     * project-state channel, rather than as a diagnostic in the buffer: the
     * source is fine, the BUILD is missing or behind, and anchoring that to a
     * range would read as a defect in the file (spec §4.3). Publishing through
     * the same snapshot also means it is shown once and updated on change, not
     * repeated per keystroke (4.3.3).
     */
    fun updateFromLint(
        compilerConfigured: Boolean,
        stream: XrefStream,
        archive: ArchiveHealth.State = ArchiveHealth.State.OK,
    ) {
        when {
            !compilerConfigured -> publish(Snapshot(State.UNAVAILABLE,
                "Cajeta compiler not configured — xref navigation is off; " +
                "locals, highlighting, folding and structure still work"))
            !stream.supported -> publish(Snapshot(State.UNAVAILABLE,
                "compiler emits xref schema major ${stream.versionMajor}; " +
                "this plugin speaks ${XrefStreamParser.SUPPORTED_MAJOR} — " +
                "records refused rather than misread"))
            // The archive states degrade rather than disable: lint ran and its
            // records are good, so the index is FRESH — what is missing is the
            // project's own declarations, and the reason says so.
            archive == ArchiveHealth.State.ABSENT ->
                publish(Snapshot(State.STALE, ArchiveHealth.reason(archive)))
            archive == ArchiveHealth.State.STALE ->
                publish(Snapshot(State.STALE, ArchiveHealth.reason(archive)))
            else -> publish(Snapshot(State.FRESH, null))
        }
    }

    companion object {
        fun getInstance(project: Project): CajetaXrefFreshness =
            project.getService(CajetaXrefFreshness::class.java)
    }
}
