package dev.cajeta.idea.buildtool

import com.intellij.build.BuildProgressListener
import com.intellij.build.DefaultBuildDescriptor
import com.intellij.build.events.BuildEvent
import com.intellij.build.events.impl.FailureResultImpl
import com.intellij.build.events.impl.FinishBuildEventImpl
import com.intellij.build.events.impl.OutputBuildEventImpl
import com.intellij.build.events.impl.SkippedResultImpl
import com.intellij.build.events.impl.StartBuildEventImpl
import com.intellij.build.events.impl.SuccessResultImpl

/** Where a running build writes its output; the bridge turns each append into an
 *  `OutputBuildEvent`. Thread-safe: stdout/stderr pumps call it concurrently. */
interface OutputSink {
    fun append(text: String, stdout: Boolean)
}

/** How a build process ended (spec §3.3). */
data class ProcessOutcome(val exitCode: Int, val cancelled: Boolean)

/** A running `cajeta` build the bridge drives: streams incremental output to the
 *  sink and reports how it ended. Abstracted so the bridge is unit-tested with a
 *  scripted fake and the production impl ([ProcessBuildTaskProcess]) wraps
 *  `ProcessBuilder`. `run` blocks until the child exits; `cancel` force-kills it. */
interface BuildTaskProcess {
    fun run(sink: OutputSink): ProcessOutcome
    fun cancel()
}

/** Parses one complete output line into a problem event for the build tree
 *  (spec §4); wired in U4 from `BuildProblemParser` (U3). Default: no problems. */
fun interface LineParser {
    fun parse(line: String, parentId: Any): ParsedProblem?
}

data class ParsedProblem(val event: BuildEvent, val isError: Boolean)

/**
 * Drives the IDE's native Build tool window for one build-routed task launch
 * (spec §3): emits a start event, streams the child's stdout/stderr as output
 * events, parses complete lines into problem events, and finishes as
 * success / failure / cancelled. Pure w.r.t. the platform *Application* — it only
 * constructs event objects and calls the listener — so it is unit-tested with a
 * recording [BuildProgressListener] and a fake [BuildTaskProcess].
 */
object CajetaBuildBridge {

    enum class Result { SUCCESS, FAILURE, CANCELLED }

    fun execute(
        listener: BuildProgressListener,
        buildId: Any,
        title: String,
        workDir: String,
        startTime: Long,
        preflight: () -> String?,
        process: BuildTaskProcess,
        lineParser: LineParser = LineParser { _, _ -> null },
        decorate: (DefaultBuildDescriptor) -> com.intellij.build.BuildDescriptor = { it },
    ): Result {
        val descriptor = decorate(DefaultBuildDescriptor(buildId, title, workDir, startTime))
        listener.onEvent(buildId, StartBuildEventImpl(descriptor, "$title running…"))

        preflight()?.let { reason ->
            finish(listener, buildId, title, FailureResultImpl(reason))
            return Result.FAILURE
        }

        var errorCount = 0
        val lock = Any()
        val outBuf = StringBuilder()
        val errBuf = StringBuilder()

        // A line the parser claims (an NDJSON diagnostic or compile-phase record)
        // is surfaced as its structured event ONLY — echoing it too would spill
        // raw JSON into the console the user reads. Everything else is echoed
        // verbatim. Echo is therefore line-buffered: a chunk without a newline
        // is held until its line completes (and flushed at exit below), since we
        // cannot know whether a partial line is structured.
        fun emitLine(line: String, stdout: Boolean) {
            val parsed = lineParser.parse(line, buildId)
            if (parsed != null) {
                listener.onEvent(buildId, parsed.event)
                if (parsed.isError) errorCount++
            } else {
                listener.onEvent(buildId, OutputBuildEventImpl(buildId, line + "\n", stdout))
            }
        }

        fun drainLines(buf: StringBuilder, stdout: Boolean) {
            var nl = buf.indexOf("\n")
            while (nl >= 0) {
                val line = buf.substring(0, nl)
                buf.delete(0, nl + 1)
                emitLine(line, stdout)
                nl = buf.indexOf("\n")
            }
        }

        val sink = object : OutputSink {
            override fun append(text: String, stdout: Boolean) = synchronized(lock) {
                val buf = if (stdout) outBuf else errBuf
                buf.append(text)
                drainLines(buf, stdout)
            }
        }

        val outcome = try {
            process.run(sink)
        } catch (e: Exception) {
            finish(listener, buildId, title, FailureResultImpl(e.message ?: "failed to run cajeta"))
            return Result.FAILURE
        }

        // Flush trailing partial lines (output not terminated by a newline):
        // parse them like any other line, echo them when they aren't structured.
        synchronized(lock) {
            if (outBuf.isNotEmpty()) emitLine(outBuf.toString(), stdout = true)
            if (errBuf.isNotEmpty()) emitLine(errBuf.toString(), stdout = false)
            outBuf.clear()
            errBuf.clear()
        }

        val result = when {
            outcome.cancelled -> Result.CANCELLED
            outcome.exitCode == 0 && errorCount == 0 -> Result.SUCCESS
            else -> Result.FAILURE
        }
        finish(listener, buildId, title, when (result) {
            Result.SUCCESS -> SuccessResultImpl()
            Result.CANCELLED -> SkippedResultImpl()
            Result.FAILURE -> FailureResultImpl()
        })
        return result
    }

    private fun finish(listener: BuildProgressListener, buildId: Any, title: String, result: com.intellij.build.events.EventResult) {
        listener.onEvent(buildId, FinishBuildEventImpl(buildId, null, System.currentTimeMillis(), title, result))
    }
}
