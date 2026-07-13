package dev.cajeta.idea.xref

import com.intellij.openapi.components.Service
import com.intellij.openapi.project.Project
import dev.cajeta.idea.lint.XrefStream
import dev.cajeta.idea.lint.XrefStreamParser
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

    val state: State get() = snap.get().state
    val reason: String? get() = snap.get().reason

    /** ide-features Unit 5's gate (9.1.6). */
    fun safeForRefactoring(): Boolean = state == State.FRESH

    fun refreshStarted() { snap.set(Snapshot(State.REFRESHING, null)) }
    fun refreshSucceeded() { snap.set(Snapshot(State.FRESH, null)) }
    fun refreshFailed(reason: String) { snap.set(Snapshot(State.STALE, reason)) }

    /** Per-edit lint outcome → state. Called by the annotator's apply(). */
    fun updateFromLint(compilerConfigured: Boolean, stream: XrefStream) {
        when {
            !compilerConfigured -> snap.set(Snapshot(State.UNAVAILABLE,
                "Cajeta compiler not configured — xref navigation is off; " +
                "locals, highlighting, folding and structure still work"))
            !stream.supported -> snap.set(Snapshot(State.UNAVAILABLE,
                "compiler emits xref schema major ${stream.versionMajor}; " +
                "this plugin speaks ${XrefStreamParser.SUPPORTED_MAJOR} — " +
                "records refused rather than misread"))
            else -> snap.set(Snapshot(State.FRESH, null))
        }
    }

    companion object {
        fun getInstance(project: Project): CajetaXrefFreshness =
            project.getService(CajetaXrefFreshness::class.java)
    }
}
