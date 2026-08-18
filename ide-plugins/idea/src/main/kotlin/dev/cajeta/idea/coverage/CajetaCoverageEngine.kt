package dev.cajeta.idea.coverage

import com.intellij.coverage.CoverageAnnotator
import com.intellij.coverage.CoverageEngine
import com.intellij.coverage.CoverageFileProvider
import com.intellij.coverage.CoverageRunner
import com.intellij.coverage.CoverageSuite
import com.intellij.coverage.CoverageSuitesBundle
import com.intellij.coverage.view.CoverageViewExtension
import com.intellij.execution.configurations.RunConfigurationBase
import com.intellij.execution.configurations.coverage.CoverageEnabledConfiguration
import com.intellij.openapi.components.service
import com.intellij.openapi.project.Project
import com.intellij.psi.PsiFile
import dev.cajeta.idea.CajetaFileType
import dev.cajeta.idea.buildtool.CajetaTaskRunConfiguration
import dev.cajeta.idea.debugger.CajetaRunConfiguration

/**
 * Cajeta coverage, as IntelliJ's coverage subsystem.
 *
 * Everything the user sees — gutter markers, the Coverage tool window, switching
 * between runs, closing a run, the project-view percentages — is the platform's
 * own, and none of it is reimplemented here (spec §1.4.1). This class only says
 * what coco data means: which files it applies to, how a run is loaded, and how
 * the numbers roll up.
 *
 * It deliberately uses no CLion-only API, so it works on IntelliJ IDEA Community
 * (spec §1.5.4) — the coverage extension points live in the platform module
 * `com.intellij.modules.coverage`, not in the Java coverage plugin.
 */
class CajetaCoverageEngine : CoverageEngine() {

    override fun getPresentableText(): String = "Cajeta Coverage"

    override fun isApplicableTo(conf: RunConfigurationBase<*>): Boolean =
        conf is CajetaTaskRunConfiguration || conf is CajetaRunConfiguration

    override fun createCoverageEnabledConfiguration(
        conf: RunConfigurationBase<*>,
    ): CoverageEnabledConfiguration = CajetaCoverageEnabledConfiguration(
        conf, CoverageRunner.getInstance(CajetaCoverageRunner::class.java),
    )

    override fun createCoverageSuite(
        name: String,
        project: Project,
        runner: CoverageRunner,
        fileProvider: CoverageFileProvider,
        timestamp: Long,
    ): CoverageSuite = CajetaCoverageSuite(name, project, runner, fileProvider, timestamp)

    override fun createCoverageSuite(config: CoverageEnabledConfiguration): CoverageSuite? {
        val project = config.configuration.project
        val runner = config.coverageRunner ?: return null
        return CajetaCoverageSuite(
            config.createSuiteName(), project, runner,
            config.createFileProvider(), config.createTimestamp(),
        )
    }

    override fun createEmptyCoverageSuite(runner: CoverageRunner): CoverageSuite =
        CajetaCoverageSuite()

    override fun getCoverageAnnotator(project: Project): CoverageAnnotator =
        project.service<CajetaCoverageAnnotatorService>().annotator

    /**
     * Gutters are drawn in `.cajeta` files only, and only where the file still
     * holds the content the run measured.
     *
     * The staleness half is spec §5.2 / plan 5.3.a: coverage is never drawn
     * against source it was not measured on. Once a line has been inserted, every
     * marking below it points at the wrong line, and a wrong green line is worse
     * than no line — nothing downstream can tell it is wrong. Suppressing is per
     * FILE, so editing one file leaves its neighbours' markings intact (5.1.c).
     */
    override fun coverageEditorHighlightingApplicableTo(psiFile: PsiFile): Boolean {
        if (!isCajetaFile(psiFile)) return false
        val path = psiFile.virtualFile?.path ?: return true
        return !CocoFreshness.getInstance(psiFile.project).isStale(path)
    }

    private fun isCajetaFile(psiFile: PsiFile): Boolean =
        psiFile.fileType == CajetaFileType ||
            psiFile.virtualFile?.extension.equals(CajetaFileType.defaultExtension, ignoreCase = true)

    override fun acceptedByFilters(psiFile: PsiFile, suite: CoverageSuitesBundle): Boolean =
        coverageEditorHighlightingApplicableTo(psiFile)

    override fun coverageProjectViewStatisticsApplicableTo(
        fileOrDir: com.intellij.openapi.vfs.VirtualFile,
    ): Boolean = fileOrDir.isDirectory ||
        fileOrDir.extension.equals(CajetaFileType.defaultExtension, ignoreCase = true)

    override fun createCoverageViewExtension(
        project: Project,
        suiteBundle: CoverageSuitesBundle,
    ): CoverageViewExtension = CajetaCoverageViewExtension(
        project, getCoverageAnnotator(project), suiteBundle,
    )
}

/**
 * The annotator is per project and must outlive any single suite — the platform
 * calls [CoverageEngine.getCoverageAnnotator] repeatedly and compares the result
 * with the one that holds the current rollup.
 */
@com.intellij.openapi.components.Service(com.intellij.openapi.components.Service.Level.PROJECT)
class CajetaCoverageAnnotatorService(project: Project) {
    val annotator: CajetaCoverageAnnotator = CajetaCoverageAnnotator(project)
}

/**
 * Coverage settings for a Cajeta run configuration.
 *
 * Unit 3 loads runs from disk; wiring "Run with Coverage" to produce them is
 * Unit 4, which is what fills in the output path.
 */
class CajetaCoverageEnabledConfiguration(
    configuration: RunConfigurationBase<*>,
    runner: CoverageRunner,
) : CoverageEnabledConfiguration(configuration, runner)
