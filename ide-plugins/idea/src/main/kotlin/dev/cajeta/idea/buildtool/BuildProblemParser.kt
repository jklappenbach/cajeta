package dev.cajeta.idea.buildtool

import com.intellij.build.FilePosition
import com.intellij.build.events.BuildEvent
import com.intellij.build.events.MessageEvent
import com.intellij.build.events.impl.FileMessageEventImpl
import com.intellij.build.events.impl.MessageEventImpl
import dev.cajeta.idea.lint.CajetacRunner
import dev.cajeta.idea.lint.Diagnostic
import java.io.File

/**
 * One compiler diagnostic destined for the Build tool window's problem tree
 * (spec §4). Shares the degraded-lint severity vocabulary (constraint §1.4.2).
 * `file`/`line`/`column` are present only when the compiler's output carried a
 * path (line/column are 1-based as the compiler prints them).
 */
data class BuildProblem(
    val severity: Diagnostic.Severity,
    val message: String,
    val file: String? = null,
    val line: Int? = null,
    val column: Int? = null,
) {
    val isError: Boolean get() = severity == Diagnostic.Severity.ERROR

    private fun kind(): MessageEvent.Kind = when (severity) {
        Diagnostic.Severity.ERROR -> MessageEvent.Kind.ERROR
        Diagnostic.Severity.WARNING, Diagnostic.Severity.WEAK_WARNING -> MessageEvent.Kind.WARNING
    }

    /** Adapt to a Build API event for the [CajetaBuildBridge] `LineParser` seam.
     *  A resolvable path yields a navigable [FileMessageEventImpl]; otherwise a
     *  positionless [MessageEventImpl] (spec §4.3–4.4). Platform `FilePosition`
     *  is 0-based, so the compiler's 1-based line is decremented. */
    fun toParsed(parentId: Any): ParsedProblem {
        val event: BuildEvent = if (file != null && line != null) {
            val pos = FilePosition(File(file), (line - 1).coerceAtLeast(0), (column ?: 0).coerceAtLeast(0))
            FileMessageEventImpl(parentId, kind(), GROUP, message, message, pos)
        } else {
            MessageEventImpl(parentId, kind(), GROUP, message, message)
        }
        return ParsedProblem(event, isError)
    }

    companion object { const val GROUP = "cajeta" }
}

/**
 * Regex parser over the compiler's unstructured diagnostics — the interim
 * degraded path until `cajeta --diag-format=json` lands (spec §4, non-goal
 * §1.5.1). Stateful: the `CajetaLogger` error is a `<path>[l:c]` line followed by
 * an `Error <id>: <msg>` line, so a location line is held pending until the
 * message line completes it. Not thread-safe — the bridge feeds it from one
 * drain path at a time.
 */
class BuildProblemParser {

    var sawError: Boolean = false
        private set

    private var pending: PendingLocation? = null

    fun feed(rawLine: String): BuildProblem? {
        val line = rawLine.trimEnd()

        // A pending CajetaLogger location expects the very next line to be its
        // `Error <id>: <msg>`; pair them, else drop the stale location.
        pending?.let { loc ->
            ERROR_MSG_RE.find(line)?.let { m ->
                pending = null
                return error(m.groupValues[2], file = loc.path, line = loc.line, column = loc.col)
            }
            pending = null
        }

        WARNING_RE.matchEntire(line.trim())?.let { m ->
            val id = m.groups["id"]!!.value
            val msg = m.groups["msg"]!!.value
            return BuildProblem(Diagnostic.Severity.WARNING, "[$id] $msg")
        }

        LOGGER_LOC_RE.find(line)?.let { m ->
            pending = PendingLocation(m.groupValues[1], m.groupValues[2].toInt(), m.groupValues[3].toInt())
            return null
        }

        ANTLR_ERR_RE.matchEntire(line)?.let { m ->
            return error(line.trim(), line = m.groupValues[1].toInt(), column = m.groupValues[2].toInt())
        }

        return null
    }

    private fun error(message: String, file: String? = null, line: Int? = null, column: Int? = null): BuildProblem {
        sawError = true
        return BuildProblem(Diagnostic.Severity.ERROR, message, file, line, column)
    }

    private data class PendingLocation(val path: String, val line: Int, val col: Int)

    companion object {
        // Reuse the degraded-lint warning grammar verbatim (constraint §1.4.2).
        private val WARNING_RE = CajetacRunner.WARNING_RE

        // `<path>[<l>:<c>]` or `[<l>,<c>]`, optionally behind a glog `E…] ` header.
        private val LOGGER_LOC_RE =
            Regex("""(?:^[EWIF]\d{4}\s.*?]\s)?(\S.*?)\[(\d+)[:,](\d+)]\s*$""")

        // The continuation line: `Error <id>: <msg>`.
        private val ERROR_MSG_RE = Regex("""Error\s+([^:]+):\s*(.+)$""")

        // ANTLR default syntax error: `line <l>:<c> <msg>` (no path).
        private val ANTLR_ERR_RE = Regex("""^line\s+(\d+):(\d+)\s+(.+)$""")
    }
}
