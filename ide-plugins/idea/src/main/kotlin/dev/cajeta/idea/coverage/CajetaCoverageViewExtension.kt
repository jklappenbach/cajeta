package dev.cajeta.idea.coverage

import com.intellij.coverage.CoverageAnnotator
import com.intellij.coverage.CoverageSuitesBundle
import com.intellij.coverage.view.DirectoryCoverageViewExtension
import com.intellij.coverage.view.ElementColumnInfo
import com.intellij.coverage.view.PercentageCoverageColumnInfo
import com.intellij.ide.util.treeView.AbstractTreeNode
import com.intellij.openapi.project.Project
import com.intellij.util.ui.ColumnInfo

/**
 * The Coverage tool window's tree for a file-oriented language.
 *
 * [DirectoryCoverageViewExtension] already gives the directory tree, the
 * navigation and the sorting; this adds the two columns coco can fill that the
 * base class does not know about. A single percentage hides the case worth
 * seeing: every line executed, half the decisions never tested.
 */
class CajetaCoverageViewExtension(
    project: Project,
    private val annotator: CoverageAnnotator,
    bundle: CoverageSuitesBundle,
) : DirectoryCoverageViewExtension(project, annotator, bundle) {

    override fun createColumnInfos(): Array<ColumnInfo<*, *>> = arrayOf(
        ElementColumnInfo(),
        PercentageCoverageColumnInfo(LINES, "Lines, %", mySuitesBundle),
        PercentageCoverageColumnInfo(BRANCHES, "Branches, %", mySuitesBundle),
        PercentageCoverageColumnInfo(METHODS, "Methods, %", mySuitesBundle),
    )

    override fun getPercentage(columnIndex: Int, node: AbstractTreeNode<*>): String? {
        val file = extractFile(node) ?: return null
        val a = annotator as? CajetaCoverageAnnotator ?: return null
        val m = (if (file.isDirectory) a.metricsForDirectory(file) else a.metricsFor(file))
            ?: return null
        return when (columnIndex) {
            LINES -> format(m.linePercent, m.coveredLines, m.totalLines)
            BRANCHES -> if (m.totalBranches == 0) null
            else format(m.branchPercent, m.coveredBranches, m.totalBranches)
            METHODS -> format(m.functionPercent, m.coveredFunctions, m.totalFunctions)
            else -> null
        }
    }

    private fun format(percent: Int, covered: Int, total: Int): String =
        "$percent% ($covered/$total)"

    private companion object {
        const val LINES = 1
        const val BRANCHES = 2
        const val METHODS = 3
    }
}
