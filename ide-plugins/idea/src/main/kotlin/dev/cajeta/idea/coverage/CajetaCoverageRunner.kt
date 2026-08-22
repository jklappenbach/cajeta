package dev.cajeta.idea.coverage

import com.intellij.coverage.CoverageEngine
import com.intellij.coverage.CoverageRunner
import com.intellij.coverage.CoverageSuite
import com.intellij.openapi.diagnostic.logger
import com.intellij.openapi.project.Project
import com.intellij.rt.coverage.data.ProjectData
import java.io.File

/**
 * Loads a coco run off disk.
 *
 * The runner is the "how do I read this file" half of the coverage API; the
 * engine is the "what does it mean for this language" half. Everything about
 * coco's formats lives behind [CocoArtifacts], so this is only file location,
 * path resolution and error reporting.
 */
class CajetaCoverageRunner : CoverageRunner() {

    override fun getPresentableName(): String = "Cajeta (coco)"

    override fun getId(): String = ID

    // Deprecated in favour of getDataFileExtensions(), but still abstract, so
    // it must be implemented. The plural defaults to a list of this one.
    @Suppress("OVERRIDE_DEPRECATION")
    override fun getDataFileExtension(): String = "profile"

    override fun acceptsCoverageEngine(engine: CoverageEngine): Boolean =
        engine is CajetaCoverageEngine

    override fun canBeLoaded(file: File): Boolean =
        file.isFile && CocoArtifacts.locateSiteTable(file) != null

    override fun loadCoverageData(sessionDataFile: File, baseCoverageSuite: CoverageSuite?): ProjectData? {
        val project = baseCoverageSuite?.project
        return try {
            val coverage = CocoArtifacts.load(sessionDataFile)
            if (coverage == null) {
                // Not an error worth a dialog: an artifact directory without a
                // site table is an incomplete run, not a corrupt one.
                LOG.info(
                    "coco: no ${CocoArtifacts.SITE_TABLE_NAME} beside or above " +
                        "${sessionDataFile.path}; nothing to load"
                )
                return null
            }
            val resolver = CocoPathResolver.forProfile(sessionDataFile, sourceRoots(project))
            // A site path that resolves to nothing keys ClassData by a relative
            // string, which the platform's annotator never matches — so the run
            // loads, reports its percentage, and paints NOTHING. That is the
            // failure this warning exists to name; it is invisible otherwise.
            val unresolved = coverage.files.filterNot { File(resolver.resolve(it)).isAbsolute }
            if (unresolved.isNotEmpty()) {
                LOG.warn(
                    "coco: ${unresolved.size} of ${coverage.files.size} measured files did not " +
                        "resolve against the project's source roots (first: ${unresolved.first()}); " +
                        "those files will show no gutters. Check \"src\" in cajeta.json."
                )
            }
            CocoProjectData.toProjectData(coverage, resolver::resolve)
        } catch (e: CocoFormatException) {
            // Refusing is the whole point: probe ids are positional against the
            // site table, so a file read under the wrong assumptions reports
            // lines green that never ran.
            LOG.warn("coco: refusing ${sessionDataFile.path}: ${e.message}")
            null
        } catch (e: java.io.IOException) {
            LOG.warn("coco: cannot read ${sessionDataFile.path}", e)
            null
        }
    }

    private fun sourceRoots(project: Project?): List<File> = CocoSourceRoots.of(project)

    companion object {
        const val ID: String = "cajeta-coco"
        private val LOG = logger<CajetaCoverageRunner>()
    }
}
