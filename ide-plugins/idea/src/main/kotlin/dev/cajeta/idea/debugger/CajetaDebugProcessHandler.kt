package dev.cajeta.idea.debugger

import com.intellij.execution.process.ProcessHandler
import com.intellij.execution.process.ProcessOutputTypes
import com.intellij.execution.ui.ConsoleView
import com.intellij.execution.ui.ConsoleViewContentType
import com.intellij.openapi.editor.markup.TextAttributes
import com.intellij.openapi.util.Key
import com.intellij.ui.JBColor
import java.awt.Color
import java.awt.Font
import java.io.OutputStream

/**
 * ProcessHandler representing the debuggee carried by `cajeta dap`. The DAP
 * server runs the program in-process, so there's no console pipe to attach;
 * this handler exists to drive the debugger UI's lifecycle (running ->
 * terminated) and to surface any program output forwarded over DAP `output`
 * events.
 */
class CajetaDebugProcessHandler : ProcessHandler() {

    /** Invoked when the user stops/detaches; used to disconnect the DAP session. */
    @Volatile
    var onDestroy: (() -> Unit)? = null

    override fun destroyProcessImpl() {
        onDestroy?.invoke()
        notifyProcessTerminated(0)
    }

    override fun detachProcessImpl() {
        onDestroy?.invoke()
        notifyProcessDetached()
    }

    override fun detachIsDefault(): Boolean = false

    override fun getProcessInput(): OutputStream? = null

    fun reportTerminated(exitCode: Int) {
        if (!isProcessTerminated) notifyProcessTerminated(exitCode)
    }

    /** Program stdout — green. */
    fun emitOutput(text: String) = emit(text, CAJETA_STDOUT)

    /** Program/server stderr — red. */
    fun emitError(text: String) = emit(text, ProcessOutputTypes.STDERR)

    /** Launch narration ("cajeta: using cached build") — plain, not program output. */
    fun emitNarration(text: String) = emit(text, ProcessOutputTypes.SYSTEM)

    // --- console attachment + pre-attach replay -------------------------
    //
    // The platform will not attach a console to us: on CLion 2026.2
    // XDebugProcess.createConsole() returns a console it never called
    // attachToProcess on, and the split-debugger session path adds nothing.
    // CajetaDebugProcess.createConsole therefore routes through attachConsole.
    //
    // Attachment happens when the platform builds the UI, which is after
    // sessionInitialized has already emitted the launch narration (and, on a
    // warm session, the program's first output). notifyTextAvailable before a
    // listener exists is dropped, so that text is buffered here and printed
    // into the console at attach.

    private val replayLock = Any()
    private val replay = ArrayList<Pair<String, Key<*>>>()
    private var replaying = true

    private fun emit(text: String, type: Key<*>) {
        synchronized(replayLock) {
            if (replaying && replay.size < MAX_REPLAY_LINES) replay.add(text to type)
            notifyTextAvailable(text, type)
        }
    }

    /**
     * Attach [console] to this handler and flush anything emitted before now.
     *
     * Attach and drain happen under one lock so concurrent output can neither
     * be lost (emitted after the drain, before the attach) nor doubled
     * (emitted after the attach, then replayed).
     */
    fun attachConsole(console: ConsoleView) {
        synchronized(replayLock) {
            console.attachToProcess(this)
            if (replaying) {
                replay.forEach { (text, type) ->
                    console.print(text, ConsoleViewContentType.getConsoleViewType(type))
                }
                replay.clear()
                replaying = false
            }
        }
    }

    companion object {
        /**
         * Cap on buffered pre-attach output. A cold build's narration is a few
         * dozen lines; this bounds the case where no console ever attaches.
         */
        const val MAX_REPLAY_LINES = 2000

        /**
         * Key for program stdout. Deliberately NOT ProcessOutputTypes.STDOUT:
         * console content types are registered per key in a global registry, so
         * coloring the platform key would repaint stdout in every console in
         * the IDE, not just ours.
         */
        val CAJETA_STDOUT: Key<Any> = Key.create("CAJETA_STDOUT")

        /** Green: the debuggee's own stdout, distinct from launch narration. */
        val STDOUT_CONTENT: ConsoleViewContentType = ConsoleViewContentType(
            "CAJETA_STDOUT",
            TextAttributes(
                JBColor(Color(0x00701A), Color(0x6A8759)),
                null, null, null, Font.PLAIN,
            ),
        )

        init {
            ConsoleViewContentType.registerNewConsoleViewType(CAJETA_STDOUT, STDOUT_CONTENT)
        }
    }
}
