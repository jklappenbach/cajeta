package dev.cajeta.idea.coverage

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
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

    // --- the versioned form the engine's mutate action writes ---------------

    @Test
    fun aVersionedDocumentParsesTheSameAsTheLegacyOne() {
        // `cajeta.coverage.mutate` writes `coco-mutation v1` above the column
        // header; the shell driver's older output starts at the header. Both
        // describe the same rows and must read identically, or upgrading coco
        // would silently change what the Mutants tab shows.
        val versioned = CocoMutation.VERSION + "\n" + doc
        assertEquals(all().size, CocoMutation.parse(versioned).size)
        assertEquals(
            all().map { it.verdict },
            CocoMutation.parse(versioned).map { it.verdict },
        )
        assertEquals(all().map { it.method }, CocoMutation.parse(versioned).map { it.method })
    }

    @Test
    fun anUnsupportedMutationVersionIsRefusedNotGuessedAt() {
        // The rule coco's other three formats follow: a reader that does not
        // recognise a version refuses the file. Parsing what it recognises and
        // skipping the rest yields a plausible wrong number, which nothing
        // downstream can detect.
        val future = "coco-mutation v2\n" + doc
        val e = assertThrows(CocoFormatMutationException::class.java) {
            CocoMutation.parse(future)
        }
        assertTrue(
            "names the version it saw and the one it reads: ${e.message}",
            e.message!!.contains("v2") && e.message!!.contains(CocoMutation.VERSION),
        )
    }

    @Test
    fun aVersionMarkerWithNoColumnHeaderIsRefused() {
        val e = assertThrows(CocoFormatMutationException::class.java) {
            CocoMutation.parse(CocoMutation.VERSION + "\n")
        }
        assertTrue("says what is missing: ${e.message}", e.message!!.contains("header"))
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

    // --- 8.3.a  a real run: covered line, surviving mutant -------------------

    @Test
    fun aRealRunShowsACoveredLineWhoseMutantSurvived() {
        // Not hand-made. `coco mutate` over a project that deliberately
        // under-asserts one boundary (see PROVENANCE.md).
        //
        // The row that matters is `atLeast` line 13, `if (n >= limit)`. Its
        // coverage run reports 2 line hits with BOTH branch arms taken — 100%
        // line AND branch coverage — and the sge->sgt mutant survived anyway.
        // Every coverage metric calls that line fully tested; nobody pinned its
        // behaviour. That gap is the whole argument for this tab.
        val real = javaClass.getResourceAsStream("/coco/mutation/mutation.tsv")!!
            .bufferedReader().readText()
        val all = CocoMutation.parse(real)
        assertEquals(6, all.size)

        val survivor = CocoMutation.survivors(all)
            .single { it.method.contains("atLeast") }
        assertEquals(13, survivor.srcLine)
        assertEquals("sge->sgt", survivor.mutation)
        assertEquals("probe/Guard.cajeta", survivor.sourceFile)

        // The control: the same SHAPE of comparison, properly asserted, dies.
        val killed = all.single { it.method.contains("isPositive") }
        assertEquals(MutationVerdict.KILLED, killed.verdict)
        assertEquals("sgt->sge", killed.mutation)

        // 6.4.3 — and none of this is the uncovered case. coco reported
        // "0 skipped as uncovered": every mutant ran, so a survivor here means
        // execution without verification, not absence of execution.
        assertTrue(
            "nothing was skipped for lack of coverage",
            all.none { it.verdict == MutationVerdict.SKIPPED_UNCOVERED },
        )
        assertTrue(CocoMutation.summarize(all).contains("3/6 killed"))
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
            CocoMutation.NOT_AVAILABLE.contains("separate pass"),
        )
        assertTrue(
            "names the action that produces it: ${CocoMutation.NOT_AVAILABLE}",
            CocoMutation.NOT_AVAILABLE.contains("cajeta.coverage.mutate"),
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
