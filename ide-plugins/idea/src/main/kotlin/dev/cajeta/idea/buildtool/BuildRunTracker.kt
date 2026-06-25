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

    fun register(handler: ProcessHandler) {
        active += handler
        handler.addProcessListener(object : ProcessListener {
            override fun processTerminated(event: ProcessEvent) { active.remove(handler) }
        })
        // Guard against a handler that finished before the listener attached.
        if (handler.isProcessTerminated) active.remove(handler)
    }

    fun activeCount(): Int = active.size

    /** Cancel every active run (graceful, then forced by the handler). */
    fun stopAll() {
        for (h in active) h.destroyProcess()
    }

    companion object {
        fun getInstance(project: Project): BuildRunTracker = project.service()
    }
}
