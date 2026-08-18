package dev.cajeta.idea.coverage

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.nio.file.Files

/**
 * ide-coverage-plan Unit 7.1.e / 7.1.f / 7.3.a — the risk ranking, read.
 *
 * The point of these is as much what they DON'T do: no CRAP score is computed
 * anywhere on this side. coco owns the metric, and a second implementation in
 * Kotlin would be a second definition — two definitions of a metric drift while
 * both keep producing plausible numbers.
 */
class CocoCrapTest {

    private val doc = "coco-crap v1\n" +
        "demo.Parser.parse(String)\t12\t83\t1183\n" +
        "demo.Cache.evict(int32)\t7\t500\t295\n" +
        "demo.Box.get()\t1\t1000\t10\n"

    // --- 6.3.1  worst first, and the file's order IS the ranking -------------

    @Test
    fun theRankingIsPresentedInTheOrderCocoEmittedIt() {
        // coco sorts worst-first. Re-sorting here would be re-deciding the
        // ranking, which is exactly the thing being deferred to coco.
        assertEquals(
            listOf("demo.Parser.parse(String)", "demo.Cache.evict(int32)", "demo.Box.get()"),
            CocoCrap.parse(doc).map { it.method },
        )
    }

    // --- 6.3.3  the inputs are visible so the number is explicable -----------

    @Test
    fun eachEntryShowsTheComplexityAndCoverageBehindItsScore() {
        val worst = CocoCrap.parse(doc).first()
        assertEquals(12, worst.complexity)
        assertEquals(8, worst.coveragePercent)
        assertEquals("118.3", worst.score)
        val text = worst.explain()
        assertTrue("names complexity: $text", text.contains("complexity 12"))
        assertTrue("names coverage: $text", text.contains("8%"))
    }

    @Test
    fun theConventionalThresholdOfThirtySeparatesHighRisk() {
        val e = CocoCrap.parse(doc)
        assertTrue("118.3 is high risk", e[0].isHighRisk)
        assertFalse("29.5 is below 30", e[1].isHighRisk)
        assertFalse("1.0 is fine", e[2].isHighRisk)
    }

    @Test
    fun scoresAreRenderedFromTheIntegerCocoPublishedNotRecomputed() {
        // Tenths in, tenths out. Reports are diffed in CI; re-deriving through a
        // float would introduce drift that reads as a real change.
        assertEquals("29.5", CocoCrap.parse(doc)[1].score)
        assertEquals("1.0", CocoCrap.parse(doc)[2].score)
    }

    // --- refuse, do not cope --------------------------------------------------

    @Test
    fun anUnknownVersionIsRefusedNamingWhatItFound() {
        val e = runCatching { CocoCrap.parse("coco-crap v2\n") }.exceptionOrNull()
        assertTrue(e is CocoFormatCrapException)
        assertTrue(e!!.message!!.contains("coco-crap v2"))
        assertTrue(e.message!!.contains("coco-crap v1"))
    }

    @Test
    fun aMalformedRowIsRefusedWithItsLineNumber() {
        val e = runCatching { CocoCrap.parse("coco-crap v1\ndemo.A.b()\t1\n") }.exceptionOrNull()
        assertTrue(e is CocoFormatCrapException)
        assertTrue(e!!.message!!.contains("line 2"))
    }

    // --- 6.2.4's shape: absent is stated -------------------------------------

    @Test
    fun aRunWithoutARankingReturnsNullSoTheViewCanSaySo() {
        val base = Files.createTempDirectory("coco-crap").toFile()
        File(base, "run").mkdirs()
        File(base, "sites.tsv").writeText("coco-sites v1\n")
        val profile = File(base, "run/coco.profile").also { it.writeText("coco-profile v1\n") }
        assertNull(CocoCrap.beside(profile))
        assertTrue(CocoCrap.NOT_AVAILABLE.contains("report action"))
    }

    @Test
    fun aRankingBesideTheSiteTableIsFound() {
        val base = Files.createTempDirectory("coco-crap2").toFile()
        File(base, "run").mkdirs()
        File(base, "sites.tsv").writeText("coco-sites v1\n")
        File(base, CocoCrap.FILE_NAME).writeText(doc)
        val profile = File(base, "run/coco.profile").also { it.writeText("coco-profile v1\n") }
        assertEquals(3, CocoCrap.beside(profile)!!.size)
    }

    // --- the formula is NOT reimplemented here -------------------------------

    @Test
    fun noScoreIsComputedOnThisSideAtAll() {
        // A guard against the obvious future "improvement". If a Kotlin CRAP
        // formula ever appears, this reader will have two sources of truth.
        val src = File("src/main/kotlin/dev/cajeta/idea/coverage/CocoCrap.kt").readText()
        for (fragment in listOf("complexity * complexity", "Math.pow", "1000 - cov")) {
            assertFalse("the metric must not be recomputed here: found `$fragment`",
                src.contains(fragment))
        }
    }
}
