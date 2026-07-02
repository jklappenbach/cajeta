package dev.cajeta.idea.buildtool

import com.intellij.execution.process.ProcessEvent
import com.intellij.execution.process.ProcessHandler
import com.intellij.execution.process.ProcessListener
import com.intellij.openapi.components.Service
import com.intellij.openapi.components.service
import com.intellij.openapi.project.Project
import java.util.concurrent.CopyOnWriteArrayList

/**
 * Tracks the tool window's live task runs so the toolbar Stop can cancel them
 * and report whether any are active (spec §9.2.3). Project-scoped; a run
 * configuration registers its [ProcessHandler] when it starts and is dropped
 * automatically when the process terminates.
 */
@Service(Service.Level.PROJECT)
class BuildRunTracker {

    private val active = CopyOnWriteArrayList<ProcessHandler>()
    // Build-window launches aren't ProcessHandler-backed (they run through
    // CajetaBuildBridge); they register a cancel hook so the toolbar Stop and
    // activeCount cover them too (spec §5.4).
    private val cancelables = CopyOnWriteArrayList<Cancelable>()

    fun interface Cancelable { fun cancel() }

    fun register(handler: ProcessHandler) {
        active += handler
        handler.addProcessListener(object : ProcessListener {
            override fun processTerminated(event: ProcessEvent) { active.remove(handler) }
        })
        // Guard against a handler that finished before the listener attached.
        if (handler.isProcessTerminated) active.remove(handler)
    }

    fun registerCancelable(c: Cancelable) { cancelables += c }
    fun unregisterCancelable(c: Cancelable) { cancelables.remove(c) }

    fun activeCount(): Int = active.size + cancelables.size

    /** Cancel every active run (graceful, then forced by the handler). */
    fun stopAll() {
        for (h in active) h.destroyProcess()
        for (c in cancelables) c.cancel()
    }

    companion object {
        fun getInstance(project: Project): BuildRunTracker = project.service()
    }
}
