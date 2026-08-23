package dev.cajeta.idea.coverage

import com.intellij.openapi.actionSystem.AnActionEvent
import com.intellij.openapi.actionSystem.ActionUpdateThread
import com.intellij.openapi.ide.CopyPasteManager
import com.intellij.openapi.project.DumbAwareAction
import com.intellij.openapi.project.Project
import java.awt.datatransfer.StringSelection

/**
 * The cross-tab actions (Tier 2) — the ones that make the four views one tool.
 *
 * Each answers the question its finding raises, and each states the limits of
 * its answer rather than rounding it up. coco's attribution is TRUNCATED per
 * line, so a join over it can under-report; saying "at least N" when that is
 * what is known is the difference between a tool that is trusted and one that
 * is quietly wrong.
 */
object CocoCrossActions {

    /** Mutants ▸ the tests that ran this line. */
    class TestsCoveringMutant(
        private val project: Project,
        private val selection: () -> MutantResult?,
    ) : DumbAwareAction("Tests Covering This Line") {

        override fun getActionUpdateThread() = ActionUpdateThread.BGT

        override fun update(e: AnActionEvent) {
            e.presentation.isEnabled = selection() != null &&
                CocoAnalysis.getInstance(project).perTest != null
        }

        override fun actionPerformed(e: AnActionEvent) {
            val mutant = selection() ?: return
            val model = CocoAnalysis.getInstance(project).perTest ?: return
            val covering = CocoCrossLinks.testsCovering(model, mutant)
            if (covering == null || covering.tests.isEmpty()) {
                // A mutant on a line NO test ran is a different finding: it is
                // skipped-uncovered, not a survivor, and the fix is a test
                // rather than an assertion. Say that instead of opening an
                // empty tab.
                CocoNotify.info(
                    project,
                    "No test covered ${mutant.sourceFile}:${mutant.srcLine} — " +
                        "this line needs a test before a mutant on it means anything.",
                )
                return
            }
            val names = covering.tests.toSet()
            val found = CocoTabs.revealIn(project, CocoTabs.TESTS) {
                it is CocoTestSummary && it.name in names
            }
            CocoNotify.info(project, describeCovering(covering, found))
        }
    }

    /** Tests ▸ the survivors on the lines this test covers. */
    class SurvivorsForTest(
        private val project: Project,
        private val selection: () -> CocoTestSummary?,
    ) : DumbAwareAction("Surviving Mutants on This Test's Lines") {

        override fun getActionUpdateThread() = ActionUpdateThread.BGT

        override fun update(e: AnActionEvent) {
            val analysis = CocoAnalysis.getInstance(project)
            e.presentation.isEnabled = selection() != null &&
                analysis.perTest != null && analysis.mutants != null
        }

        override fun actionPerformed(e: AnActionEvent) {
            val test = selection() ?: return
            val analysis = CocoAnalysis.getInstance(project)
            val survivors = CocoCrossLinks.survivorsOnLinesOf(
                analysis.perTest, analysis.mutants, test.name,
            )
            if (survivors.isEmpty()) {
                CocoNotify.info(project, describeNoSurvivors(analysis.perTest, test))
                return
            }
            val keys = survivors.map { it.sourceFile to it.srcLine }.toSet()
            val found = CocoTabs.revealIn(project, CocoTabs.MUTANTS) {
                it is MutantResult && (it.sourceFile to it.srcLine) in keys
            }
            CocoNotify.info(project, describeSurvivors(analysis.perTest, test, survivors.size, found))
        }
    }

    /** Risk ▸ the first line the run never reached. */
    class GoToFirstUncoveredLine(
        private val project: Project,
        private val selection: () -> CrapEntry?,
    ) : DumbAwareAction("Go to First Uncovered Line") {

        override fun getActionUpdateThread() = ActionUpdateThread.BGT

        override fun update(e: AnActionEvent) {
            e.presentation.isEnabled = selection() != null
        }

        override fun actionPerformed(e: AnActionEvent) {
            val entry = selection() ?: return
            val coverage = CocoAnalysis.getInstance(project).coverage ?: return
            val site = CocoCrossLinks.firstUncoveredLine(coverage, entry.method)
            if (site == null) {
                // Every line ran. The score is complexity, not coverage — the
                // work is a decomposition, not a test, and sending someone
                // hunting for an uncovered line would waste their time.
                CocoNotify.info(
                    project,
                    "${entry.method} has no uncovered line — its CRAP score is " +
                        "complexity, not coverage (${entry.explain()}).",
                )
                return
            }
            CocoNavigation.open(project, site.file, site.line)
        }
    }

    /** Risk ▸ the score's inputs, for a ticket or a commit message. */
    class CopyExplanation(
        private val selection: () -> CrapEntry?,
    ) : DumbAwareAction("Copy Explanation") {

        override fun getActionUpdateThread() = ActionUpdateThread.BGT

        override fun update(e: AnActionEvent) {
            e.presentation.isEnabled = selection() != null
        }

        override fun actionPerformed(e: AnActionEvent) {
            val entry = selection() ?: return
            CopyPasteManager.getInstance()
                .setContents(StringSelection("${entry.method} — ${entry.explain()}"))
        }
    }

    // ── message text, separated so it is assertable without Swing ──────────

    fun describeCovering(covering: TestsForLine, revealed: Int): String {
        val where = "${covering.file}:${covering.line}"
        if (revealed < 0) return "Could not open the Tests tab."
        return if (covering.isTruncated) {
            "$where is covered by ${covering.testCount} tests; coco lists " +
                "${covering.tests.size} of them and $revealed are selected. " +
                "The list is a sample, not the full set."
        } else {
            "$where is covered by ${covering.testCount} test(s); $revealed selected."
        }
    }

    fun describeSurvivors(
        model: CocoAttributionModel?,
        test: CocoTestSummary,
        survivors: Int,
        revealed: Int,
    ): String {
        if (revealed < 0) return "Could not open the Mutants tab."
        val complete = CocoCrossLinks.linesAreComplete(model, test.name)
        val count = if (complete) "$survivors" else "at least $survivors"
        return "$count surviving mutant(s) on lines ${test.name} covers" +
            if (complete) "." else " — attribution for some of those lines is " +
                "truncated, so this is a lower bound."
    }

    fun describeNoSurvivors(model: CocoAttributionModel?, test: CocoTestSummary): String {
        val complete = CocoCrossLinks.linesAreComplete(model, test.name)
        val base = "No surviving mutant on lines ${test.name} covers"
        return if (complete) {
            "$base. Nothing here argues for keeping it — but coverage overlap " +
                "is evidence of redundancy, never proof."
        } else {
            "$base in the attribution coco recorded — some of those lines have " +
                "truncated test lists, so this is not proof that it kills nothing."
        }
    }
}
