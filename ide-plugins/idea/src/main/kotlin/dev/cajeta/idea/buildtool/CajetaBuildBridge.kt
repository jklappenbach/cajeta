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

/**
 * Parses one complete output line into build-tree events (spec §4); wired in U4
 * from `BuildProblemParser` (U3). An empty list means "not structured" — the
 * bridge then echoes the line as plain console output.
 *
 * A line may yield SEVERAL events: the first compile phase both opens the
 * "Compile" grouping node and opens itself under it. [close] is called once the
 * process has exited, so a parser can shut any node it left open (the grouping
 * node) before the build finishes — an unclosed node renders as still-running.
 */
fun interface LineParser {
    fun parse(line: String, parentId: Any): List<ParsedProblem>
    fun close(parentId: Any): List<ParsedProblem> = emptyList()
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
        lineParser: LineParser = LineParser { _, _ -> emptyList() },
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
            if (parsed.isEmpty()) {
                // Render-stream, not arrival-stream: the build tool writes
                // plugin progress to stderr, which the Build window paints red.
                // See ConsoleStreamClassifier.
                val renderStdout = ConsoleStreamClassifier.renderAsStdout(line, stdout)
                listener.onEvent(buildId, OutputBuildEventImpl(buildId, line + "\n", renderStdout))
                return
            }
            for (p in parsed) {
                listener.onEvent(buildId, p.event)
                if (p.isError) errorCount++
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
            // Close nodes the parser still holds open (the "Compile" group) BEFORE
            // the build's own finish event, or they render as forever-running.
            for (p in lineParser.close(buildId)) listener.onEvent(buildId, p.event)
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
