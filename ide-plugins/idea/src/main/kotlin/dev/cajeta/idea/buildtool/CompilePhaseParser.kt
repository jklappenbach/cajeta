package dev.cajeta.idea.buildtool

import com.intellij.build.events.BuildEvent
import com.intellij.build.events.MessageEvent
import com.intellij.build.events.impl.FinishEventImpl
import com.intellij.build.events.impl.MessageEventImpl
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
}

/**
 * A cache-hit record (`BuildAction`, `kind: "cache"`): the build tool re-published
 * [artifact] from its cache and never ran the compiler. Reported because such a
 * build emits no phases at all — an instant green check under an empty tree is
 * indistinguishable from a build that silently did nothing.
 */
data class CacheHit(val artifact: String)

/**
 * Parses one NDJSON record from the compiler/build-tool stream. Returns null for
 * anything else — a diagnostic (which has `severity` and no `kind`), a plain
 * console line, or malformed JSON — so the caller can fall through to the
 * diagnostic parser and then to raw output. Pure; mirrors [JsonDiagnosticParser].
 */
object CompilePhaseParser {

    fun parse(line: String): CompilePhase? = obj(line)?.let { root ->
        if ((root.opt("kind") as? Json.Str)?.value != "progress") return null
        val phase = (root.opt("phase") as? Json.Str)?.value ?: return null
        val state = when ((root.opt("state") as? Json.Str)?.value) {
            "start" -> CompilePhase.State.START
            "finish" -> CompilePhase.State.FINISH
            else -> return null
        }
        CompilePhase(
            phase = phase,
            state = state,
            // Fall back to the stable id when a future compiler omits the label.
            label = (root.opt("label") as? Json.Str)?.value ?: phase,
            elapsedMs = (root.opt("elapsedMs") as? Json.Num)?.value?.toLong(),
        )
    }

    fun parseCacheHit(line: String): CacheHit? = obj(line)?.let { root ->
        if ((root.opt("kind") as? Json.Str)?.value != "cache") return null
        CacheHit(artifact = (root.opt("artifact") as? Json.Str)?.value ?: "")
    }

    private fun obj(line: String): Json.Obj? {
        val trimmed = line.trim()
        if (trimmed.isEmpty() || trimmed[0] != '{') return null
        val root = try {
            Json.parse(trimmed)
        } catch (_: Exception) {
            return null
        }
        return root as? Json.Obj
    }
}

/**
 * The line parser the Build tool window runs: compile phases first, then
 * diagnostics, else "not structured" (empty) so the bridge echoes the line as
 * plain console output.
 *
 * Phases accumulate as a tree — each one stays in the view after it completes,
 * with the time it took:
 *
 *     Compile
 *       Scanning sources    899 ms
 *       Parsing             9.3 s
 *       Resolving types     76 ms
 *       Generating code     ⟳            ← running
 *
 * The compiler reports a phase's duration on its `finish` record, so the elapsed
 * time is written into the node's final title rather than inferred from event
 * timestamps (which would measure pipe-read latency, not compiler work).
 *
 * The "Compile" node is opened lazily by the first phase and closed by [close]
 * when the process exits — a node left open renders as forever-running.
 */
class BuildOutputParser : LineParser {

    private val problems = BuildProblemParser()
    private var compileOpened = false

    val sawError: Boolean get() = problems.sawError

    override fun parse(line: String, parentId: Any): List<ParsedProblem> {
        CompilePhaseParser.parse(line)?.let { return phaseEvents(it, parentId) }
        CompilePhaseParser.parseCacheHit(line)?.let { return cacheEvents(it, parentId) }
        return problems.feed(line)?.toParsed(parentId)?.let { listOf(it) } ?: emptyList()
    }

    /**
     * A cached build runs no compiler, so it reports no phases. Say so in the
     * tree — an instant green check with nothing under it reads as a broken
     * build. Rendered as an INFO message node, not a phase: nothing was compiled,
     * so there is no duration to report.
     */
    private fun cacheEvents(hit: CacheHit, parentId: Any): List<ParsedProblem> {
        val text = if (hit.artifact.isEmpty()) "Up to date — restored from cache"
                   else "Up to date — ${hit.artifact} restored from cache (no compilation)"
        return listOf(
            ParsedProblem(
                MessageEventImpl(parentId, MessageEvent.Kind.INFO, COMPILE, text, text),
                isError = false,
            )
        )
    }

    /** Close the grouping node if any phase ever opened it. Idempotent. */
    override fun close(parentId: Any): List<ParsedProblem> {
        if (!compileOpened) return emptyList()
        compileOpened = false
        return listOf(
            ParsedProblem(
                FinishEventImpl(
                    CompileNodeId(parentId), parentId, System.currentTimeMillis(),
                    COMPILE, SuccessResultImpl(),
                ),
                isError = false,
            )
        )
    }

    private fun phaseEvents(p: CompilePhase, parentId: Any): List<ParsedProblem> {
        val compileId = CompileNodeId(parentId)
        val phaseId = PhaseId(parentId, p.phase)
        val now = System.currentTimeMillis()
        val events = mutableListOf<BuildEvent>()

        // The first phase to arrive opens the grouping node the phases hang under.
        if (!compileOpened) {
            compileOpened = true
            events += StartEventImpl(compileId, parentId, now, COMPILE)
        }
        events += when (p.state) {
            // A phase closes as SUCCESS even when the build ultimately fails: it
            // marks "the compiler moved past this phase", and errors surface as
            // their own problem nodes. The compiler's RAII marker closes a phase
            // even while a fatal diagnostic unwinds, so nothing spins forever.
            CompilePhase.State.FINISH ->
                FinishEventImpl(
                    phaseId, compileId, now,
                    p.elapsedMs?.let { "${p.label}  ${humanMs(it)}" } ?: p.label,
                    SuccessResultImpl(),
                )
            CompilePhase.State.START -> StartEventImpl(phaseId, compileId, now, p.label)
        }
        return events.map { ParsedProblem(it, isError = false) }
    }

    /** `899 ms`, `9.3 s`, `1 m 04 s` — the unit the number deserves. */
    private fun humanMs(ms: Long): String = when {
        ms < 1_000 -> "$ms ms"
        ms < 60_000 -> String.format("%.1f s", ms / 1000.0)
        else -> String.format("%d m %02d s", ms / 60_000, (ms % 60_000) / 1000)
    }

    /** Node ids: equal across a start/finish pair (that is what pairs them into one
     *  node) and scoped to the build, so concurrent builds never close each other's. */
    private data class CompileNodeId(val buildId: Any)
    private data class PhaseId(val buildId: Any, val phase: String)

    private companion object { const val COMPILE = "Compile" }
}
