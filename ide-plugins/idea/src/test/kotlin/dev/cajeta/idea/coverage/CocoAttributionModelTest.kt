package dev.cajeta.idea.coverage

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.nio.file.Files

/**
 * ide-coverage-plan Unit 7.1.a–d — the attribution query layer.
 *
 * The READER was validated against coco's published document in Unit 2; what is
 * under test here is what the queries mean, so the input is built directly.
 */
class CocoAttributionModelTest {

    private val doc = "# coco-attribution v1\n" +
        "# test\tAlpha\tcovered=3\tunique=1\n" +
        "# test\tBeta\tcovered=2\tunique=0\n" +
        "A.cajeta\t10\t1\tAlpha\n" +
        "A.cajeta\t11\t2\tAlpha|Beta\n" +
        "A.cajeta\t12\t1\tBeta\n" +
        "B.cajeta\t5\t9\tAlpha|Beta|Gamma|+6\n"

    private fun model() = CocoAttributionModel(parseCocoAttribution(doc))

    // --- 6.2.1  which tests covered a line ------------------------------------

    @Test
    fun theTestsCoveringALineAreListed() {
        val t = model().testsCovering("A.cajeta", 11)!!
        assertEquals(listOf("Alpha", "Beta"), t.tests)
        assertEquals(2, t.testCount)
        assertTrue(!t.isTruncated)
    }

    @Test
    fun aTruncatedListSaysHowManyItOmitsRatherThanLookingComplete() {
        // coco caps the list. Rendering the sample as the whole set would be a
        // quiet lie about which tests cover the line.
        val t = model().testsCovering("B.cajeta", 5)!!
        assertTrue(t.isTruncated)
        assertEquals("the count is authoritative", 9, t.testCount)
        assertEquals(6, t.omitted)
        assertTrue("the description admits it: ${t.describe()}", t.describe().contains("6 more"))
        assertTrue(t.describe().contains("9 total"))
    }

    @Test
    fun aLineNoTestCoversIsAbsentRatherThanEmpty() {
        assertNull(model().testsCovering("A.cajeta", 99))
    }

    // --- 6.2.2  a test's uniquely-covered lines -------------------------------

    @Test
    fun aTestsUniquelyCoveredLinesAreThoseNoOtherTestReaches() {
        assertEquals(listOf(10), model().uniqueLinesOf("Alpha").map { it.line })
        assertEquals(listOf(12), model().uniqueLinesOf("Beta").map { it.line })
    }

    @Test
    fun aSharedLineIsUniqueToNobody() {
        // Line 11 is covered by both, so it belongs to neither's unique set.
        assertTrue(model().uniqueLinesOf("Alpha").none { it.line == 11 })
        assertTrue(model().uniqueLinesOf("Beta").none { it.line == 11 })
    }

    @Test
    fun aTruncatedLineIsNeverAttributedUniquely() {
        // The only provably-complete list is a one-entry one. Attributing a
        // truncated line to the first name in its sample would invent a fact.
        assertTrue(model().uniqueLinesOf("Alpha").none { it.file == "B.cajeta" })
    }

    // --- 6.2.3  redundancy candidates -----------------------------------------

    @Test
    fun aTestWithNoUniqueCoverageIsARedundancyCandidate() {
        assertEquals(listOf("Beta"), model().redundancyCandidates().map { it.name })
    }

    // --- 6.2.4  absent attribution is stated ----------------------------------

    @Test
    fun aRunWithoutAttributionReturnsNullSoTheViewCanSaySo() {
        // Not an empty model: an empty table reads as "no test covers anything",
        // which is a different and alarming claim.
        val base = Files.createTempDirectory("coco-attr").toFile()
        File(base, "run").mkdirs()
        File(base, "sites.tsv").writeText("coco-sites v1\n")
        val profile = File(base, "run/coco.profile").also { it.writeText("coco-profile v1\n") }
        assertNull(CocoAttributionModel.beside(profile))
        assertTrue(CocoAttributionModel.NOT_COLLECTED.contains("did not collect"))
    }

    @Test
    fun anAttributionBesideTheSiteTableIsFound() {
        val base = Files.createTempDirectory("coco-attr2").toFile()
        File(base, "run").mkdirs()
        File(base, "sites.tsv").writeText("coco-sites v1\n")
        File(base, CocoAttributionModel.FILE_NAME).writeText(doc)
        val profile = File(base, "run/coco.profile").also { it.writeText("coco-profile v1\n") }
        val m = CocoAttributionModel.beside(profile)
        assertNotNull(m)
        assertEquals(2, m!!.tests.size)
    }
}
