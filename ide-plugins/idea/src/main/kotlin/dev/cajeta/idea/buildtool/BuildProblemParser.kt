package dev.cajeta.idea.buildtool

import com.intellij.build.FilePosition
import com.intellij.build.events.BuildEvent
import com.intellij.build.events.MessageEvent
import com.intellij.build.events.impl.FileMessageEventImpl
import com.intellij.build.events.impl.MessageEventImpl
import dev.cajeta.idea.lint.Diagnostic
import java.io.File

/**
 * One compiler diagnostic destined for the Build tool window's problem tree
 * (json-diagnostics-spec §5). Shares the degraded-lint severity vocabulary.
 * `file`/`line`/`column` are present only when the compiler's NDJSON carried
 * them (line/column 1-based, as emitted).
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
     *  positionless [MessageEventImpl]. Platform `FilePosition` is 0-based, so the
     *  compiler's 1-based line is decremented. */
    fun toParsed(parentId: Any): ParsedProblem {
        val event: BuildEvent = if (file != null && line != null) {
            val pos = FilePosition(File(file), (line - 1).coerceAtLeast(0), (column ?: 1).minus(1).coerceAtLeast(0))
            FileMessageEventImpl(parentId, kind(), GROUP, message, message, pos)
        } else {
            MessageEventImpl(parentId, kind(), GROUP, message, message)
        }
        return ParsedProblem(event, isError)
    }

    companion object { const val GROUP = "cajeta" }
}

/**
 * Turns the compiler's `--diag-format=json` NDJSON (which the build tool forwards,
 * json-diagnostics-spec §2/§5) into build-tree problems. Delegates to the shared
 * [JsonDiagnosticParser]: one line per diagnostic, so no regex, no cross-line
 * state. A non-NDJSON stderr line is not a diagnostic — it stays plain console
 * output (parse returns null; no regex fallback).
 */
class BuildProblemParser {

    var sawError: Boolean = false
        private set

    fun feed(line: String): BuildProblem? {
        val d = JsonDiagnosticParser.parse(line) ?: return null
        if (d.severity == Diagnostic.Severity.ERROR) sawError = true
        val message = if (!d.code.isNullOrBlank()) "[${d.code}] ${d.message}" else d.message
        return BuildProblem(d.severity, message, d.file, d.line, d.column)
    }
}
