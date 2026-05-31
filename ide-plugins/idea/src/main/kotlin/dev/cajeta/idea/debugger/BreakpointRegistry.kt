package dev.cajeta.idea.debugger

import java.util.TreeSet

/**
 * Tracks the set of line breakpoints per source file. DAP setBreakpoints is a
 * whole-file replace, so the registry keeps the current line set for each file
 * and hands back the updated, sorted list on every add/remove. Plain and
 * thread-safe so the XBreakpointHandler can mutate it off any thread; the
 * snapshot feeds the launch handshake.
 */
class BreakpointRegistry {

    private val linesByFile = HashMap<String, TreeSet<Int>>()

    /** Add a 1-based line for [file]; returns that file's current sorted lines. */
    @Synchronized
    fun add(file: String, line: Int): List<Int> {
        linesByFile.getOrPut(file) { TreeSet() }.add(line)
        return linesByFile.getValue(file).toList()
    }

    /** Remove a 1-based line for [file]; returns that file's remaining lines. */
    @Synchronized
    fun remove(file: String, line: Int): List<Int> {
        val set = linesByFile[file] ?: return emptyList()
        set.remove(line)
        if (set.isEmpty()) linesByFile.remove(file)
        return set.toList()
    }

    /** Current lines for [file] (sorted, possibly empty). */
    @Synchronized
    fun linesFor(file: String): List<Int> = linesByFile[file]?.toList() ?: emptyList()

    /** Immutable snapshot of every file's breakpoint lines. */
    @Synchronized
    fun snapshot(): Map<String, List<Int>> =
        linesByFile.mapValues { it.value.toList() }
}
