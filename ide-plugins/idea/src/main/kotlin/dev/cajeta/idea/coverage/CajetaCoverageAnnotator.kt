package dev.cajeta.idea.coverage

import com.intellij.coverage.BaseCoverageAnnotator
import com.intellij.coverage.CoverageDataManager
import com.intellij.coverage.CoverageSuitesBundle
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.rt.coverage.data.ClassData
import com.intellij.rt.coverage.data.LineData
import java.io.File

/**
 * Line, branch and method counts for one file or directory.
 *
 * All three are kept because coco measures all three, and because a single
 * "coverage %" hides the interesting cases: a file at 100% lines and 50%
 * branches has every line executed and half its decisions never tested.
 */
data class CocoFileMetrics(
    val totalLines: Int = 0,
    val coveredLines: Int = 0,
    val totalBranches: Int = 0,
    val coveredBranches: Int = 0,
    val totalFunctions: Int = 0,
    val coveredFunctions: Int = 0,
) {
    operator fun plus(o: CocoFileMetrics) = CocoFileMetrics(
        totalLines + o.totalLines, coveredLines + o.coveredLines,
        totalBranches + o.totalBranches, coveredBranches + o.coveredBranches,
        totalFunctions + o.totalFunctions, coveredFunctions + o.coveredFunctions,
    )

    val linePercent: Int get() = percent(coveredLines, totalLines)
    val branchPercent: Int get() = percent(coveredBranches, totalBranches)
    val functionPercent: Int get() = percent(coveredFunctions, totalFunctions)

    private fun percent(covered: Int, total: Int): Int =
        if (total == 0) 100 else (covered * 100.0 / total).toInt()
}

/**
 * Per-file and per-directory rollups for the Coverage tool window and the
 * project view.
 *
 * Built on [BaseCoverageAnnotator] rather than `SimpleCoverageAnnotator`, which
 * tracks line counts only: coco has branch and method data and dropping it here
 * would be throwing away the measurement that most often changes a decision.
 *
 * A file the run never mentions returns **null**, not 0% — reporting zero for a
 * file coco never instrumented is a false negative that reads exactly like a
 * real one (spec §3.6).
 */
class CajetaCoverageAnnotator(project: Project) : BaseCoverageAnnotator(project) {

    /** Absolute, `/`-separated path → that file's metrics. */
    @Volatile
    private var byFile: Map<String, CocoFileMetrics> = emptyMap()

    override fun onSuiteChosen(newSuite: CoverageSuitesBundle?) {
        super.onSuiteChosen(newSuite)
        byFile = emptyMap()
    }

    override fun createRenewRequest(
        suite: CoverageSuitesBundle,
        dataManager: CoverageDataManager,
    ): Runnable = Runnable { byFile = collect(suite, dataManager) }

    /** Metrics for one file, or null when the run says nothing about it. */
    fun metricsFor(file: VirtualFile): CocoFileMetrics? = byFile[key(file.path)]

    /** Metrics for everything under [dir], or null when it holds no measured file. */
    fun metricsForDirectory(dir: VirtualFile): CocoFileMetrics? {
        val prefix = key(dir.path).trimEnd('/') + "/"
        var total: CocoFileMetrics? = null
        for ((path, m) in byFile) {
            if (!path.startsWith(prefix)) continue
            total = total?.plus(m) ?: m
        }
        return total
    }

    override fun getFileCoverageInformationString(
        project: Project,
        file: VirtualFile,
        bundle: CoverageSuitesBundle,
        manager: CoverageDataManager,
    ): String? = metricsFor(file)?.let { "${it.linePercent}% lines covered" }

    override fun getDirCoverageInformationString(
        project: Project,
        directory: VirtualFile,
        bundle: CoverageSuitesBundle,
        manager: CoverageDataManager,
    ): String? = metricsForDirectory(directory)?.let {
        "${it.linePercent}% lines covered"
    }

    private fun collect(
        bundle: CoverageSuitesBundle,
        dataManager: CoverageDataManager,
    ): Map<String, CocoFileMetrics> {
        val out = HashMap<String, CocoFileMetrics>()
        for (suite in bundle.suites) {
            val data = suite.getCoverageData(dataManager) ?: continue
            for (cd in data.classesCollection) {
                val name = cd.name ?: continue
                val metrics = metricsOf(cd)
                out.merge(key(name), metrics) { a, b -> a + b }
            }
        }
        return out
    }

    /**
     * Paths arrive from two directions — coco's resolved absolute paths and the
     * VFS — so they are compared on a single spelling: `/`-separated, and
     * case-folded only where the filesystem itself is.
     */
    private fun key(path: String): String {
        val slashed = path.replace(File.separatorChar, '/')
        return if (CASE_INSENSITIVE_FS) slashed.lowercase() else slashed
    }

    private companion object {
        private val CASE_INSENSITIVE_FS: Boolean =
            System.getProperty("os.name").orEmpty().lowercase().let {
                it.startsWith("windows") || it.startsWith("mac")
            }

        fun metricsOf(cd: ClassData): CocoFileMetrics {
            var totalLines = 0
            var coveredLines = 0
            var totalBranches = 0
            var coveredBranches = 0

            for (o in cd.lines ?: emptyArray()) {
                val ld = o as? LineData ?: continue
                totalLines++
                if (ld.hits > 0) coveredLines++
                for (jump in ld.jumps ?: emptyArray()) {
                    // Two arms per decision; each is covered on its own.
                    totalBranches += 2
                    if (jump.trueHits > 0) coveredBranches++
                    if (jump.falseHits > 0) coveredBranches++
                }
            }

            val sigs = cd.methodSigs
            val coveredFunctions = sigs.count { (cd.getStatus(it) ?: 0) != 0 }
            return CocoFileMetrics(
                totalLines = totalLines,
                coveredLines = coveredLines,
                totalBranches = totalBranches,
                coveredBranches = coveredBranches,
                totalFunctions = sigs.size,
                coveredFunctions = coveredFunctions,
            )
        }
    }
}
