package dev.cajeta.idea.profiler

import com.intellij.openapi.project.Project
import dev.cajeta.idea.debugger.Json
import dev.cajeta.idea.xref.XrefQuery

/**
 * A totals row navigates to the METHOD, not to a sample.
 *
 * A trace records the line a frame was on WHEN THE SLICE OPENED — the statement
 * that happened to be executing at that tick. The trace writer says as much:
 * "the slice says where it was first seen, not where it spent its time". On the
 * tour profile every one of 53 frames resolved past its own declaration, a
 * median of 6 lines in and as much as 146 (`svdCore` at 929, declared at 783).
 *
 * For a flame node that is at least a fact about that slice. A totals row is an
 * aggregate of every occurrence of a name across every track, so a single
 * sample's line is arbitrary — there is no "where" for a sum. Reported
 * 2026-09-01: "clicks on Totals entries are taken to the middle of the method
 * and not the beginning".
 *
 * The frame descriptor carries no declaration line ({typeName, methodName,
 * fileName} and nothing else), so the line cannot come from the trace. It comes
 * from the xref index instead — the same `declarationsOf` Ctrl-click uses, which
 * is the right source anyway: a totals row names a method, and resolving a
 * method to its declaration is what that index is for.
 */
object TotalsNavigation {

    /**
     * A frame name to the FQN the index knows it by: type arguments dropped,
     * because the index holds DECLARATIONS and a declaration has type
     * parameters, not arguments (`Optional<int32>.get` is declared as
     * `cajeta.lang.Optional.get`).
     *
     * Balanced, so a nested argument list does not truncate the name early —
     * `HashMap<int32,ArrayList<String>>.put` has to survive intact.
     *
     * Null for a lambda: `<lambda>` is a synthesized frame with no declaration
     * to find, and stripping it as if it were a type argument would leave a
     * trailing dot and send a meaningless query.
     */
    fun declarationFqn(frameName: String): String? {
        if (frameName.isEmpty()) return null
        if (frameName.contains("<lambda>")) return null
        val out = StringBuilder()
        var depth = 0
        for (c in frameName) {
            when (c) {
                '<' -> depth++
                '>' -> if (depth > 0) depth--
                else -> if (depth == 0) out.append(c)
            }
        }
        val fqn = out.toString()
        return if (fqn.isEmpty() || fqn.endsWith('.')) null else fqn
    }

    /**
     * What to look up, in order. A CONSTRUCTOR is spelled `Type.Type` in a
     * frame name and may not be indexed under that FQN, so the enclosing type
     * is tried second — the class declaration is still the beginning of the
     * thing the row names, and is far closer than an arbitrary sample.
     */
    fun lookupChain(fqn: String): List<String> {
        val chain = mutableListOf(fqn)
        val lastDot = fqn.lastIndexOf('.')
        if (lastDot > 0) {
            val owner = fqn.substring(0, lastDot)
            val simple = fqn.substring(lastDot + 1)
            // Only for a constructor — otherwise a method that is simply not
            // indexed would silently open its class, which looks like a hit.
            if (owner.substringAfterLast('.') == simple) chain.add(owner)
        }
        return chain
    }

    private fun strOf(o: Json.Obj, field: String): String? =
        (o.opt(field) as? Json.Str)?.value

    private fun intOf(o: Json.Obj, field: String): Int? =
        (o.opt(field) as? Json.Num)?.value?.toInt()

    /**
     * Open the declaration for [frameName]. False when the index has nothing —
     * the caller then falls back to the trace's own line and SAYS it did, since
     * landing mid-method without explanation is the confusion this exists to
     * remove.
     */
    fun open(project: Project, frameName: String): Boolean {
        val fqn = declarationFqn(frameName) ?: return false
        for (q in lookupChain(fqn)) {
            val decls = try {
                XrefQuery.declarationsOf(project, q)
            } catch (e: Exception) {
                // Index not ready (dumb mode) is not an error worth a dialog;
                // the trace line still works.
                emptyList()
            }
            for (d in decls) {
                val file = strOf(d, "file") ?: continue
                val line = intOf(d, "line") ?: continue
                // Reuse the profiler's own root resolution — project roots plus
                // the mounted stdlib — so a stdlib declaration opens for the
                // same reason a stdlib frame does.
                if (ProfileNavigation.open(project, ProfileSourceLocation(0, file, q, line))) {
                    return true
                }
            }
        }
        return false
    }
}
