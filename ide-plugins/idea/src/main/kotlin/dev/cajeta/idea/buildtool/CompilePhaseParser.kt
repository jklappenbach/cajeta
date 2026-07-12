package dev.cajeta.idea.buildtool

import com.intellij.build.events.BuildEvent
import com.intellij.build.events.impl.FinishEventImpl
import com.intellij.build.events.impl.StartEventImpl
import com.intellij.build.events.impl.SuccessResultImpl
import dev.cajeta.idea.debugger.Json

/**
 * One compile-phase progress record from the `--diag-format=json` NDJSON stream
 * (`Diagnostics.h::emitJsonProgress`). The compiler brackets each phase of
 * `Compiler::compile` with a `start` and a `finish` record, so the Build tool
 * window can show what the compiler is doing instead of a bare spinner: without
 * these it prints nothing at all between launch and exit.
 *
 * `elapsedMs` is present on `finish` only.
 */
data class CompilePhase(
    val phase: String,
    val state: State,
    val label: String,
    val elapsedMs: Long? = null,
) {
    enum class State { START, FINISH }

    /**
     * Adapt to the Build API. A phase is a child node of the build: `start` opens
     * it, `finish` closes it as a success. The node id must be equal across the
     * pair (that is what pairs them), and distinct per build — hence the
     * (buildId, phase) key rather than the bare phase name, so two concurrent
     * builds don't close each other's nodes.
     *
     * A phase closes as SUCCESS even when the build ultimately fails: it marks
     * "the compiler moved past this phase", and errors surface as their own
     * problem nodes. The compiler's RAII marker also closes a phase while an
     * exception unwinds, so a failed build still leaves no phase spinning.
     */
    fun toParsed(parentId: Any): ParsedProblem {
        val id = PhaseId(parentId, phase)
        val now = System.currentTimeMillis()
        val event: BuildEvent = when (state) {
            State.START -> StartEventImpl(id, parentId, now, label)
            State.FINISH -> FinishEventImpl(id, parentId, now, label, SuccessResultImpl())
        }
        return ParsedProblem(event, isError = false)
    }

    private data class PhaseId(val buildId: Any, val phase: String)
}

/**
 * Parses one NDJSON progress record. Returns null for anything else — a
 * diagnostic (which has `severity` and no `kind`), a plain console line, or
 * malformed JSON — so the caller can fall through to the diagnostic parser and
 * then to raw output. Pure; mirrors [JsonDiagnosticParser]'s contract.
 */
object CompilePhaseParser {

    fun parse(line: String): CompilePhase? {
        val trimmed = line.trim()
        if (trimmed.isEmpty() || trimmed[0] != '{') return null
        val root = try {
            Json.parse(trimmed)
        } catch (_: Exception) {
            return null
        }
        if (root !is Json.Obj) return null
        if ((root.opt("kind") as? Json.Str)?.value != "progress") return null
        val phase = (root.opt("phase") as? Json.Str)?.value ?: return null
        val state = when ((root.opt("state") as? Json.Str)?.value) {
            "start" -> CompilePhase.State.START
            "finish" -> CompilePhase.State.FINISH
            else -> return null
        }
        return CompilePhase(
            phase = phase,
            state = state,
            // Fall back to the stable id when a future compiler omits the label.
            label = (root.opt("label") as? Json.Str)?.value ?: phase,
            elapsedMs = (root.opt("elapsedMs") as? Json.Num)?.value?.toLong(),
        )
    }
}

/**
 * The line parser the Build tool window runs: compile phases first, then
 * diagnostics, else "not structured" (null) so the bridge echoes the line as
 * plain console output. Stateful only in the diagnostic parser's error latch.
 */
class BuildOutputParser {

    private val problems = BuildProblemParser()

    val sawError: Boolean get() = problems.sawError

    fun parse(line: String, parentId: Any): ParsedProblem? {
        CompilePhaseParser.parse(line)?.let { return it.toParsed(parentId) }
        return problems.feed(line)?.toParsed(parentId)
    }
}
