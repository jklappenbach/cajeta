package dev.cajeta.idea.coverage

import com.intellij.coverage.CoverageDataAnnotationsManager
import com.intellij.coverage.CoverageDataManager
import com.intellij.coverage.CoverageRunner
import com.intellij.coverage.CoverageSuitesBundle
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.diagnostic.logger
import com.intellij.openapi.progress.ProcessCanceledException
import com.intellij.openapi.progress.ProgressIndicator
import com.intellij.openapi.progress.ProgressManager
import com.intellij.openapi.progress.Task
import com.intellij.openapi.project.Project
import java.io.File

/**
 * Turning a finished coco run into a loaded, displayed coverage suite.
 *
 * Shared by the Run with Coverage path and the load-from-disk action, because
 * "results exist on disk" is the same situation in both cases — one of them just
 * produced them a moment ago (spec §4.1, §4.5). Keeping one implementation is
 * also what keeps §4.4's promise honest: re-examining a run and running it are
 * genuinely the same code from here on, so re-rendering cannot quietly re-run.
 */
object CocoRunLoader {

    /** Why a load did not happen, for the caller to report as it sees fit. */
    sealed interface Outcome {
        data class Loaded(val profile: File, val suiteName: String) : Outcome
        data class NoArtifacts(val outDir: File) : Outcome
        data class Unreadable(val profile: File, val reason: String) : Outcome
    }

    /**
     * Discover, load and select the run under [outDir].
     *
     * Reads on the calling thread — the parse is EDT-free by design (2.1.h) —
     * and hands the suite to the platform on the EDT, which is where the
     * platform mutates its own state.
     */
    fun loadAndShow(project: Project, outDir: File, label: String): Outcome {
        val profile = CocoArtifacts.discoverProfile(outDir)
            ?: return Outcome.NoArtifacts(outDir)

        // Refuse rather than guess: probe ids are positional against the site
        // table, so a profile without its own table attributes hits to whatever
        // table turned up.
        if (CocoArtifacts.locateSiteTable(profile) == null) {
            return Outcome.Unreadable(
                profile,
                "no ${CocoArtifacts.SITE_TABLE_NAME} beside or above it — did instrument run?",
            )
        }
        val coverage = try {
            CocoArtifacts.load(profile)
        } catch (e: CocoFormatException) {
            return Outcome.Unreadable(profile, e.message ?: "unrecognised format")
        } catch (e: java.io.IOException) {
            return Outcome.Unreadable(profile, e.message ?: "could not be read")
        } ?: return Outcome.NoArtifacts(outDir)

        // Snapshot what the run measured, before anything can be edited. Doing
        // this at load rather than lazily is what makes "changed after the run"
        // detectable at all (spec §5).
        val resolver = CocoPathResolver.forProfile(profile, sourceRootsOf(project))
        val resolved = coverage.files.map(resolver::resolve)
        // An unresolved path is not an error anywhere downstream — it just
        // annotates no file. That silence is exactly how a loaded run came to
        // paint nothing, so say it once, with a name to grep for.
        val unresolved = coverage.files.filterIndexed { i, _ -> !File(resolved[i]).isAbsolute }
        if (unresolved.isNotEmpty()) {
            LOG.warn(
                "coco: ${unresolved.size} of ${coverage.files.size} measured files did not resolve " +
                    "to a path on disk (first: ${unresolved.first()}); those files will show no " +
                    "coverage. Check the source root in cajeta.json."
            )
        }
        CocoFreshness.getInstance(project)
            .observeRun(profile, resolved)

        // Classify while still off the EDT — this queries the xref index.
        //
        // Isolated from the load. Classification is the SECONDARY product: the
        // gutters come from the profile alone, while dead-vs-untested needs the
        // xref index and everything that can go wrong with it. A threading
        // violation in here used to take the entire load down and leave the
        // coverage window empty, which reads as "coverage is broken" rather than
        // "one tab has no data".
        try {
            CocoAnalysis.getInstance(project).update(coverage)
            CocoAnalysis.getInstance(project).updateSidecars(profile)
        } catch (e: ProcessCanceledException) {
            throw e
        } catch (e: Exception) {
            LOG.warn("coco: classification failed; loading coverage without it", e)
        }

        val manager = CoverageDataManager.getInstance(project)
        val runner = CoverageRunner.getInstance(CajetaCoverageRunner::class.java)
        val suite = manager.addExternalCoverageSuite(profile, runner)
        ApplicationManager.getApplication().invokeLater {
            if (!project.isDisposed) {
                manager.chooseSuitesBundle(CoverageSuitesBundle(suite))

                // Re-annotate the editors that are ALREADY OPEN.
                //
                // `CoverageDataAnnotationsManager` annotates on two triggers:
                // an editor opening, and its own `update()`. Choosing a bundle
                // is not one of them. So loading a run left every open editor
                // exactly as it was — and since loading coverage is something
                // you do WITH your files open, that is every editor you were
                // looking at. The tool window filled in, the percentages were
                // right, and the gutters stayed blank until the file was closed
                // and reopened.
                //
                // Diagnosed by closing and reopening one file: the gutters
                // appeared, which proved the data, the ProjectData keys and
                // every engine gate were already correct, and that the only
                // thing missing was this call.
                CoverageDataAnnotationsManager.getInstance(project).update()
            }
        }
        LOG.info("coco: loaded coverage from ${profile.path}")
        return Outcome.Loaded(profile, label)
    }

    /**
     * [loadAndShow] under a cancellable background task with a progress
     * indicator — closes plan 2.2.e, which was deferred to here precisely
     * because there was no run to hang an indicator on until now.
     *
     * Parsing a large run is not instant, and doing it on the EDT would freeze
     * the editor at exactly the moment the developer is waiting to see gutters.
     */
    fun loadInBackground(
        project: Project,
        outDir: File,
        label: String,
        onDone: (Outcome) -> Unit,
    ) {
        ProgressManager.getInstance().run(
            object : Task.Backgroundable(project, "Loading Cajeta coverage", true) {
                override fun run(indicator: ProgressIndicator) {
                    indicator.isIndeterminate = true
                    indicator.text = "Reading coverage from ${outDir.name}"
                    val outcome = loadAndShow(project, outDir, label)
                    indicator.checkCanceled()
                    onDone(outcome)
                }
            }
        )
    }

    /** Human-readable form of an outcome, for a notification or the console. */
    fun describe(outcome: Outcome): String = when (outcome) {
        is Outcome.Loaded -> "Coverage loaded from ${outcome.profile.path}"
        is Outcome.NoArtifacts ->
            "The run produced no coverage profile under ${outcome.outDir.path}. " +
                "Check that the task's \"cajeta.coverage.instrument\" action ran."
        is Outcome.Unreadable ->
            "Coverage at ${outcome.profile.path} could not be read: ${outcome.reason}"
    }

    private fun sourceRootsOf(project: Project): List<File> = CocoSourceRoots.of(project)

    private val LOG = logger<CocoRunLoader>()
}
