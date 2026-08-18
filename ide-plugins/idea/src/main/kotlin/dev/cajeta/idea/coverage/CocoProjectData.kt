package dev.cajeta.idea.coverage

import com.intellij.rt.coverage.data.ClassData
import com.intellij.rt.coverage.data.LineData
import com.intellij.rt.coverage.data.ProjectData

/**
 * Translates coco's model into the platform's `ProjectData`, which is what
 * drives the gutters, the Coverage tool window and the project-view rollups.
 *
 * ## `ClassData` is keyed by FILE PATH, not by class name
 *
 * The name reads like a qualified class name because the model came from Java,
 * but `SimpleCoverageAnnotator` — the base every file-oriented language uses —
 * builds its lookup from `ProjectData.getClasses().keySet()` run through
 * `normalizeFilePath`. Keying by `probe.Cond` would leave every file
 * unannotated with no error anywhere.
 *
 * ## Three things that are easy to get quietly wrong
 *
 *  - **A null slot in the line array means "not instrumented".** Lines coco
 *    never probed must stay null so the platform leaves them unmarked; a probed
 *    line that never ran gets a `LineData` with zero hits and is painted red.
 *    Collapsing the two produces phantom red gutters on blank lines and
 *    comments (spec §3.6).
 *  - **`fillArrays()` is required before branch status is visible.**
 *    `JumpsAndSwitches` holds jumps in a list until then, and `LineData.getStatus`
 *    reads only the array — so a partially covered decision silently reports
 *    FULL, which is a green gutter on a branch that only ever went one way.
 *  - **`function` probes carry line 0.** They feed the function metric through
 *    registered method signatures, never a `LineData`.
 */
object CocoProjectData {

    /**
     * @param resolvePath maps coco's `file` field — a path as the compiler saw
     *   it, typically relative to a source root — to the absolute path the IDE
     *   knows the file by.
     */
    fun toProjectData(coverage: CocoCoverage, resolvePath: (String) -> String): ProjectData {
        val project = ProjectData()
        for ((file, sites) in coverage.sites.groupBy { it.file }) {
            project.addClassData(classData(resolvePath(file), file, sites, coverage))
        }
        return project
    }

    private fun classData(
        path: String,
        cocoFile: String,
        sites: List<CocoSite>,
        coverage: CocoCoverage,
    ): ClassData {
        val cd = ClassData(path)
        cd.setSource(path.substringAfterLast('/').substringAfterLast('\\'))

        val sourceLines = sites.filter { it.isSourceLine }
        val maxLine = sourceLines.maxOfOrNull { it.line } ?: 0
        val lines = arrayOfNulls<LineData>(maxLine + 1)

        // One LineData per instrumented line. Its signature is that of any probe
        // on the line; a line belongs to exactly one method.
        for ((line, onLine) in sourceLines.groupBy { it.line }) {
            val ld = LineData(line, onLine.first().signature)
            ld.setHits(clamp(coverage.lineHits(cocoFile, line)))
            lines[line] = ld
        }

        // Branch arms. Each decision becomes one jump on its line, carrying the
        // arms' own counts — which is how PARTIAL is distinguished from FULL.
        //
        // The index is per LINE and must start at 0: `addJump(n)` pads the line's
        // jump list out to n+1 entries, so a running index across the file
        // manufactures empty jumps — which then read as unevaluated decisions and
        // drag every percentage down. A line can hold more than one decision
        // (`a && b` puts two on one line), hence the grouping.
        for (branch in coverage.branches().filter { it.file == cocoFile }
            .groupBy { it.line }) {
            val ld = lines.getOrNull(branch.key) ?: continue
            branch.value.forEachIndexed { index, b ->
                val jump = ld.addJump(index)
                jump.trueHits = clamp(b.trueHits)
                jump.falseHits = clamp(b.falseHits)
            }
        }
        for (ld in lines) ld?.fillArrays()

        cd.setLines(lines)

        // Registered after the lines, because getStatus(signature) derives a
        // method's coverage by scanning them. Every method coco saw is
        // registered — including one instrumented only at entry — so an
        // uncalled method is reported uncovered rather than omitted.
        for (signature in sites.map { it.signature }.distinct()) {
            cd.registerMethodSignature(LineData(0, signature))
        }
        return cd
    }

    /**
     * coco counts in 64 bits; `LineData` holds an int. Saturate, never wrap.
     *
     * Wrapping is not a small error here: the platform's own `trimHits` maps
     * anything negative to its ceiling of 1,000,000,000, so a count that
     * overflowed into the negatives comes back as "ran a billion times" — the
     * opposite of a safe failure.
     */
    private fun clamp(count: Long): Int =
        if (count > Int.MAX_VALUE) Int.MAX_VALUE else if (count < 0) 0 else count.toInt()
}
