package dev.cajeta.idea.coverage

import com.intellij.coverage.BaseCoverageSuite
import com.intellij.coverage.CoverageEngine
import com.intellij.coverage.CoverageFileProvider
import com.intellij.coverage.CoverageRunner
import com.intellij.openapi.project.Project

/**
 * One loaded coco run.
 *
 * [BaseCoverageSuite] already handles persistence, the data cache and the
 * lifecycle, so this only names its engine and declares what coco actually
 * measures: branch coverage always (coco emits `branch-true`/`branch-false`
 * pairs unconditionally), and per-test coverage only when the run produced
 * attribution.
 */
class CajetaCoverageSuite : BaseCoverageSuite {

    @Suppress("unused") // instantiated by the platform when deserializing
    constructor() : super()

    constructor(
        name: String,
        project: Project,
        runner: CoverageRunner,
        fileProvider: CoverageFileProvider,
        timestamp: Long,
    ) : super(name, project, runner, fileProvider, timestamp)

    /**
     * Always true: coco emits `branch-true`/`branch-false` pairs unconditionally,
     * so there is no non-branch mode to report. Stated here rather than through
     * the constructor flag, whose overload is deprecated.
     */
    override fun isBranchCoverage(): Boolean = true

    override fun getCoverageEngine(): CoverageEngine =
        CoverageEngine.EP_NAME.findExtensionOrFail(CajetaCoverageEngine::class.java)
}
