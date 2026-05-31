package dev.cajeta.idea.debugger

import java.util.TreeMap

/**
 * Tracks the set of line breakpoints per source file, with an optional
 * condition per line (CP6f). DAP setBreakpoints is a whole-file replace, so the
 * registry keeps the current line→condition map for each file and hands back the
 * updated, sorted view on every add/remove. Plain and thread-safe so the
 * XBreakpointHandler can mutate it off any thread; the snapshot feeds the launch
 * handshake.
 *
 * The line-only accessors ([add]/[remove]/[linesFor]/[snapshot]) are retained as
 * the primary API; the conditional variants ([breakpointsFor]/[snapshot
 * Breakpoints]) expose the condition each line carries.
 */
class BreakpointRegistry {

    // file -> (1-based line -> condition; "" = unconditional)
    private val byFile = HashMap<String, TreeMap<Int, String>>()

    /** Add a 1-based [line] for [file] with an optional [condition]; returns
     *  that file's current sorted lines. */
    @Synchronized
    fun add(file: String, line: Int, condition: String = ""): List<Int> {
        byFile.getOrPut(file) { TreeMap() }[line] = condition
        return byFile.getValue(file).keys.toList()
    }

    /** Remove a 1-based [line] for [file]; returns that file's remaining lines. */
    @Synchronized
    fun remove(file: String, line: Int): List<Int> {
        val map = byFile[file] ?: return emptyList()
        map.remove(line)
        if (map.isEmpty()) byFile.remove(file)
        return map.keys.toList()
    }

    /** Current lines for [file] (sorted, possibly empty). */
    @Synchronized
    fun linesFor(file: String): List<Int> = byFile[file]?.keys?.toList() ?: emptyList()

    /** Immutable snapshot of every file's breakpoint lines. */
    @Synchronized
    fun snapshot(): Map<String, List<Int>> =
        byFile.mapValues { it.value.keys.toList() }

    /** Current breakpoints for [file] as line+condition records (sorted). */
    @Synchronized
    fun breakpointsFor(file: String): List<CajetaDebugSession.LineBreakpoint> =
        byFile[file]?.map { (line, cond) -> CajetaDebugSession.LineBreakpoint(file, line, cond) }
            ?: emptyList()

    /** Immutable snapshot of every file's breakpoints with their conditions. */
    @Synchronized
    fun snapshotBreakpoints(): Map<String, List<CajetaDebugSession.LineBreakpoint>> =
        byFile.mapValues { (file, map) ->
            map.map { (line, cond) -> CajetaDebugSession.LineBreakpoint(file, line, cond) }
        }
}
