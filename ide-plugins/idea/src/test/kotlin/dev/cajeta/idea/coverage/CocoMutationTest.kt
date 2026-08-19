package dev.cajeta.idea.coverage

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.nio.file.Files

/**
 * ide-coverage-plan Unit 8.1 — surviving mutants.
 *
 * The distinction that matters (spec §6.4.3) is already made by coco's driver,
 * which records THREE verdicts, not two: `killed`, `SURVIVED`, and
 * `skipped-uncovered`. The last is its coverage gate — an unexecuted compare
 * cannot be killed, so it is excluded from the score rather than counted as a
 * survivor. The job here is to not collapse them.
 */
class CocoMutationTest {

    private val doc = CocoMutation.HEADER + "\n" +
        "probe/Cond.ll\t19\tslt->sle\tkilled\t_ZN5probe4Cond4loopEi\n" +
        "probe/Cond.ll\t27\teq->ne\tSURVIVED\t_ZN5probe4Cond7guardedEi\n" +
        "probe/Cond.ll\t31\tsgt->sge\tskipped-uncovered\t_ZN5probe4Cond11neverCalledEi\n" +
        "probe/Helper.ll\t5\tslt->sle\tSURVIVED\t_ZN5probe6Helper5twiceEi\n"

    private fun all() = CocoMutation.parse(doc)

    // --- 8.1.a  survivors, with location and the applied mutation ------------

    @Test
    fun survivorsAreListedWithWhereAndWhat() {
        val s = CocoMutation.survivors(all())
        assertEquals(2, s.size)
        assertEquals("probe/Cond.ll", s[0].module)
        assertEquals(27, s[0].srcLine)
        assertEquals("eq->ne", s[0].mutation)
        assertTrue("describes itself: ${s[0].describe()}", s[0].describe().contains("eq->ne"))
        assertTrue(s[0].describe().contains("27"))
    }

    @Test
    fun killedAndSkippedMutantsAreNotSurvivors() {
        val s = CocoMutation.survivors(all())
        assertTrue(s.none { it.verdict == MutationVerdict.KILLED })
        assertTrue(s.none { it.verdict == MutationVerdict.SKIPPED_UNCOVERED })
    }

    // --- 8.1.c  a survivor is NOT the same finding as an uncovered line ------

    @Test
    fun aSurvivorAndAnUncoveredSiteAreDistinctVerdicts() {
        // Execution without verification is a different problem from no
        // execution, and they have different fixes: add an assertion vs add a
        // test. Folding the second into the first would overstate the suite's
        // weakness; folding the first into the second would hide it entirely.
        val byLine = all().associateBy { it.srcLine }
        assertEquals(MutationVerdict.SURVIVED, byLine[27]!!.verdict)
        assertEquals(MutationVerdict.SKIPPED_UNCOVERED, byLine[31]!!.verdict)
    }

    @Test
    fun theSummaryCountsSkippedSeparatelyFromSurvived() {
        val text = CocoMutation.summarize(all())
        assertTrue(text, text.contains("1/3 killed"))
        assertTrue(text, text.contains("2 survived"))
        assertTrue("skipped is its own number: $text", text.contains("1 skipped as uncovered"))
    }

    @Test
    fun skippedMutantsAreExcludedFromTheScoreNotCountedAgainstIt() {
        // coco's own gate: an unexecuted compare cannot be killed, so scoring it
        // as a failure would punish the suite for a coverage gap it already
        // reports elsewhere.
        val text = CocoMutation.summarize(all())
        assertFalse("the denominator is 3, not 4: $text", text.contains("/4"))
    }

    // --- 8.1.b  navigation needs a SOURCE path, not the IR -------------------

    @Test
    fun aMutantsSourceFileIsDerivedFromItsIrModule() {
        // coco names modules by their .ll. Opening the IR would be technically
        // correct and useless.
        assertEquals("probe/Cond.cajeta", CocoMutation.survivors(all())[0].sourceFile)
        assertEquals("probe/Helper.cajeta", CocoMutation.survivors(all())[1].sourceFile)
    }

    // --- refuse, do not cope --------------------------------------------------

    @Test
    fun anUnknownVerdictIsRefusedRatherThanFoldedIntoANeighbour() {
        // Reading a new verdict as `killed` would understate the survivors —
        // the one direction that makes the suite look better than it is.
        val bad = CocoMutation.HEADER + "\nprobe/A.ll\t1\ta->b\ttimed-out\tm\n"
        val e = runCatching { CocoMutation.parse(bad) }.exceptionOrNull()
        assertTrue(e is CocoFormatMutationException)
        assertTrue(e!!.message!!.contains("timed-out"))
    }

    @Test
    fun anUnexpectedHeaderIsRefused() {
        val e = runCatching { CocoMutation.parse("module\tline\n") }.exceptionOrNull()
        assertTrue(e is CocoFormatMutationException)
    }

    @Test
    fun aMalformedRowIsRefusedWithItsLineNumber() {
        val e = runCatching {
            CocoMutation.parse(CocoMutation.HEADER + "\nprobe/A.ll\t1\n")
        }.exceptionOrNull()
        assertTrue(e is CocoFormatMutationException)
        assertTrue(e!!.message!!.contains("line 2"))
    }

    // --- 8.1.d  absent data says why -----------------------------------------

    @Test
    fun aRunWithoutMutationResultsSaysSoRatherThanShowingNothing() {
        val base = Files.createTempDirectory("coco-mut").toFile()
        File(base, "run").mkdirs()
        File(base, "sites.tsv").writeText("coco-sites v1\n")
        val profile = File(base, "run/coco.profile").also { it.writeText("coco-profile v1\n") }
        assertNull(CocoMutation.beside(profile))
        assertTrue(
            "explains that mutation is a separate pass: ${CocoMutation.NOT_AVAILABLE}",
            CocoMutation.NOT_AVAILABLE.contains("separate coco"),
        )
    }

    @Test
    fun aResultsFileBesideTheSiteTableIsFound() {
        val base = Files.createTempDirectory("coco-mut2").toFile()
        File(base, "run").mkdirs()
        File(base, "sites.tsv").writeText("coco-sites v1\n")
        File(base, CocoMutation.FILE_NAME).writeText(doc)
        val profile = File(base, "run/coco.profile").also { it.writeText("coco-profile v1\n") }
        assertEquals(4, CocoMutation.beside(profile)!!.size)
    }
}
