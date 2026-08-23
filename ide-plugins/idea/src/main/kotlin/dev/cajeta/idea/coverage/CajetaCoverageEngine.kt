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
import java.io.File
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
        val decision = decide(psiFile)
        // Logged, not silent. `CoverageDataAnnotationsManager` consults exactly
        // three engine gates before it will annotate an editor — this one,
        // `acceptedByFilters` and `isInLibraryClasses` — and a `false` from any
        // of them produces a blank gutter with no explanation anywhere. Whichever
        // one refuses, the reason is now in the log under a greppable name.
        if (!decision.applicable) {
            LOG.info("coco/gutter: NOT annotating ${decision.path} — ${decision.why}")
        } else {
            LOG.debug("coco/gutter: annotating ${decision.path}")
        }
        return decision.applicable
    }

    private data class GutterDecision(
        val applicable: Boolean,
        val path: String,
        val why: String,
    )

    private fun decide(psiFile: PsiFile): GutterDecision {
        val path = psiFile.virtualFile?.path ?: "<no virtual file>"
        if (!isCajetaFile(psiFile)) {
            return GutterDecision(
                false, path,
                "not a Cajeta file (fileType=${psiFile.fileType.name}, " +
                    "ext=${psiFile.virtualFile?.extension})",
            )
        }
        if (psiFile.virtualFile == null) {
            return GutterDecision(true, path, "no virtual file; allowed")
        }
        val freshness = CocoFreshness.getInstance(psiFile.project)
        if (freshness.isStale(path)) {
            return GutterDecision(
                false, path,
                "stale: ${freshness.reasonFor(path) ?: "content differs from the run"}",
            )
        }
        return GutterDecision(true, path, "fresh")
    }

    private val LOG = com.intellij.openapi.diagnostic.logger<CajetaCoverageEngine>()

    private fun isCajetaFile(psiFile: PsiFile): Boolean =
        psiFile.fileType == CajetaFileType ||
            psiFile.virtualFile?.extension.equals(CajetaFileType.defaultExtension, ignoreCase = true)

    override fun acceptedByFilters(psiFile: PsiFile, suite: CoverageSuitesBundle): Boolean =
        coverageEditorHighlightingApplicableTo(psiFile)

    /**
     * The keys under which this file's data sits in `ProjectData` — the step
     * that turns a loaded suite into MARKS IN THE EDITOR.
     *
     * `SrcFileAnnotator` asks the engine for a file's keys and looks each one up
     * with `ProjectData.getClassData`. The base implementation returns an EMPTY
     * SET, so an engine that does not override this loads its data, reports its
     * percentages, fills the Coverage tool window from the annotator's own
     * rollups — and paints nothing, anywhere, with no error at any layer. The
     * gutters and the tool window reach the data by different routes, and only
     * one of them was wired.
     *
     * `CocoProjectData` keys by ABSOLUTE FILE PATH (the name reads like a class
     * name for Java's sake), normalized with '/' separators, so that is what
     * this returns — and the two must be normalized identically or the lookup
     * misses on Windows while passing everywhere else.
     */
    override fun getQualifiedNames(psiFile: PsiFile): Set<String> =
        keyFor(psiFile)?.let { setOf(it) } ?: emptySet()

    /**
     * The SINGULAR form — and the one the editor actually reaches.
     *
     * `CoverageEditorAnnotatorImpl` asks for a file's output paths first; the
     * base `getCorrespondingOutputPaths` answers with the source file's own nio
     * path, so that set is never empty, so the annotator always takes the
     * output-path branch and calls THIS. The plural [getQualifiedNames] is the
     * else-branch for engines with no output paths, and is never reached here.
     *
     * The base implementation is `return null`, and a null key yields no
     * highlighters — no error, no log line. That is why coverage could load,
     * fill the tool window with correct per-file percentages (those come from
     * the annotator's own filesystem walk, not from `ProjectData` keys) and
     * still leave every gutter blank. Overriding only the plural form fixes
     * nothing, because nothing calls it.
     */
    // Widened to public deliberately: the handshake between this key and
    // `CocoProjectData`'s is the thing that was broken, and a test cannot assert
    // it through a protected member.
    public override fun getQualifiedName(outputFile: File, sourceFile: PsiFile): String? =
        keyFor(sourceFile)

    /**
     * The key `CocoProjectData` stores this file's `ClassData` under: its
     * absolute path with '/' separators. Both sides derive it here so they
     * cannot drift.
     */
    private fun keyFor(psiFile: PsiFile): String? =
        psiFile.virtualFile?.path?.replace('\\', '/')

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
