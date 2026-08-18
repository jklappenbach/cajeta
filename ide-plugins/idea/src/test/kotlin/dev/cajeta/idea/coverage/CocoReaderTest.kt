package dev.cajeta.idea.coverage

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * ide-coverage-plan Unit 2.1 — the reader, against cajeta-coco's conformance
 * fixture rather than a hand-made one (2.3.a). A fixture invented here would
 * only prove the reader agrees with itself.
 *
 * These assertions are coco's `EXPECTED.md` executed: the counts and the
 * must-reject table, checked rather than described. They also stand as the proof
 * for plan 1.3.b — this reader was written against `docs/formats.md` and the
 * fixture, without reading coco's source.
 */
class CocoReaderTest {

    private fun fixture(name: String): String =
        javaClass.getResourceAsStream("/coco/conformance/$name")
            ?.bufferedReader()?.readText()
            ?: error("missing vendored fixture: $name")

    private fun sites() = parseCocoSites(fixture("sites.tsv"))
    private fun profile() = parseCocoProfile(fixture("coco.profile"))

    // --- 2.1.a  the fixture parses into the model ----------------------------

    @Test
    fun fixtureSitesParse() {
        val s = sites()
        assertEquals("site count", 47, s.size)
        assertEquals("ids are dense from 0", (0L..46L).toList(), s.map { it.id }.sorted())
        assertEquals("kind: line", 24, s.count { it.kind == CocoSiteKind.LINE })
        assertEquals("kind: function", 7, s.count { it.kind == CocoSiteKind.FUNCTION })
        assertEquals("kind: branch-true", 8, s.count { it.kind == CocoSiteKind.BRANCH_TRUE })
        assertEquals("kind: branch-false", 8, s.count { it.kind == CocoSiteKind.BRANCH_FALSE })
        assertEquals(
            "both modules present under one header",
            listOf("probe.Cond", "probe.Helper"),
            s.map { it.owner }.distinct().sorted(),
        )
    }

    @Test
    fun fixtureProfileParses() {
        val p = profile()
        assertEquals("declared size matches the site count", 47L, p.size)
        assertEquals("hit rows", 29, p.hits.size)
        assertNull("a whole-run profile carries no test label", p.label)
        assertEquals("an absent id reads as zero, not an error", 0L, p.countOf(999L))
    }

    // --- 2.1.b  an unknown version is refused, with a message that says why ---

    @Test
    fun unknownSitesVersionIsRefused() {
        val e = runCatching { parseCocoSites("coco-sites v2\n") }.exceptionOrNull()
        assertTrue("refused", e is CocoFormatException)
        assertTrue("names the version found: ${e?.message}", e!!.message!!.contains("coco-sites v2"))
        assertTrue("names what it reads: ${e.message}", e.message!!.contains("coco-sites v1"))
    }

    @Test
    fun missingSitesHeaderIsRefused() {
        val text = "0\tfunction\t1\t-1\tA.cajeta\tp.A\tm()\tentry\t-\n"
        assertTrue(runCatching { parseCocoSites(text) }.exceptionOrNull() is CocoFormatException)
    }

    @Test
    fun unknownProfileVersionIsRefused() {
        assertTrue(
            runCatching { parseCocoProfile("coco-profile v2\nsize 1\n") }
                .exceptionOrNull() is CocoFormatException
        )
    }

    @Test
    fun malformedRowIsRefusedWithItsLineNumber() {
        val text = "coco-sites v1\n" +
            "0\tfunction\t1\t-1\tA.cajeta\tp.A\tm()\tentry\t-\n" +
            "1\tline\t2\n"
        val e = runCatching { parseCocoSites(text) }.exceptionOrNull()
        assertTrue("refused", e is CocoFormatException)
        assertTrue("reports line 3, got: ${e?.message}", e!!.message!!.contains("line 3"))
    }

    @Test
    fun unknownSiteKindIsRefusedNotCoerced() {
        val text = "coco-sites v1\n" +
            "0\tcondition-mcdc\t1\t-1\tA.cajeta\tp.A\tm()\tentry\t-\n"
        val e = runCatching { parseCocoSites(text) }.exceptionOrNull()
        assertTrue("refused rather than read as a line probe", e is CocoFormatException)
        assertTrue(e!!.message!!.contains("condition-mcdc"))
    }

    // --- 2.1.c  truncated is not malformed ------------------------------------

    @Test
    fun aTruncatedTrailingRecordIsDroppedNotRejected() {
        // No terminating newline: the writer is mid-record. Parsing stops at the
        // last complete row instead of throwing, which is what lets a live view
        // read a file while coco is still writing it.
        val whole = fixture("sites.tsv")
        val truncated = whole.substring(0, whole.length - 12)
        val s = parseCocoSites(truncated)
        assertTrue("kept the complete rows", s.size >= 45)
        assertTrue("dropped the partial one", s.size < 47)
    }

    @Test
    fun aTruncatedProfileStillParses() {
        val whole = fixture("coco.profile")
        val truncated = whole.substring(0, whole.length - 3)
        val p = parseCocoProfile(truncated)
        assertTrue("kept most hits", p.hits.size >= 27)
    }

    // --- 2.1.d  the join ------------------------------------------------------

    @Test
    fun sitesAndProfileJoinByProbeId() {
        val c = CocoCoverage(sites(), profile())
        assertEquals("29 of 47 probes hit", 29, c.coveredProbeCount())
        assertEquals("two source files", 2, c.files.size)
        for ((id, _) in profile().hits) {
            assertNotNull("every profile id resolves to a site: $id", c.siteById(id))
        }
    }

    // --- 2.1.e  max, never sum ------------------------------------------------

    @Test
    fun lineHitsTakeTheMaximumOverProbesNotTheSum() {
        val sites = listOf(
            site(0, CocoSiteKind.LINE, line = 7),
            site(1, CocoSiteKind.LINE, line = 7),
            site(2, CocoSiteKind.LINE, line = 7),
        )
        val p = CocoProfile(size = 3, label = null, hits = mapOf(0L to 4L, 1L to 4L, 2L to 4L))
        val c = CocoCoverage(sites, p)
        // Summing would say 12: a line executed four times reported as twelve.
        assertEquals(4L, c.lineHits("A.cajeta", 7))
    }

    // --- 2.1.f  never-evaluated vs evaluated-but-not-taken --------------------

    @Test
    fun anUnreachedDecisionIsDistinctFromAnUntakenArm() {
        val sites = listOf(
            site(0, CocoSiteKind.BRANCH_TRUE, line = 3, block = "b1"),
            site(1, CocoSiteKind.BRANCH_FALSE, line = 3, block = "b1"),
            site(2, CocoSiteKind.BRANCH_TRUE, line = 9, block = "b2"),
            site(3, CocoSiteKind.BRANCH_FALSE, line = 9, block = "b2"),
        )
        // decision 1 reached, only the true arm taken; decision 2 never reached.
        val p = CocoProfile(size = 4, label = null, hits = mapOf(0L to 5L))
        val b = CocoCoverage(sites, p).branches().associateBy { it.block }

        assertEquals(BranchOutcome.TAKEN, b["b1"]!!.trueOutcome)
        assertEquals("reached but not taken", BranchOutcome.NOT_TAKEN, b["b1"]!!.falseOutcome)
        assertTrue("partially covered", b["b1"]!!.isPartial)

        assertEquals("never reached", BranchOutcome.NOT_EVALUATED, b["b2"]!!.trueOutcome)
        assertEquals(BranchOutcome.NOT_EVALUATED, b["b2"]!!.falseOutcome)
        assertTrue("not partial — the decision never ran", !b["b2"]!!.isPartial)
    }

    // --- 2.1.g  guard branches are already excluded by coco -------------------

    @Test
    fun everyDecisionHasBothArms() {
        // coco excludes compiler-inserted guard branches before writing, and the
        // reader must neither expect nor re-filter them. The observable
        // consequence is that no decision has an unpaired arm — a leaked guard
        // would show up as one.
        // Paired by (file, owner, method, block). NOT by `decision` — coco
        // leaves that -1 on every row — and not by block alone, because every
        // method has an `entry` block. Both wrong keys were caught by this test.
        val arms = sites().filter { it.kind.isBranch }
        val byDecision = arms.groupBy { listOf(it.file, it.owner, it.method, it.block) }
        for ((d, a) in byDecision) {
            assertEquals("decision $d has exactly two arms", 2, a.size)
            assertEquals(1, a.count { it.kind == CocoSiteKind.BRANCH_TRUE })
            assertEquals(1, a.count { it.kind == CocoSiteKind.BRANCH_FALSE })
        }
        assertEquals("8 decisions", 8, byDecision.size)
    }

    // --- 2.1.h  parses off the EDT --------------------------------------------

    @Test
    fun parsingRunsOffTheUiThread() {
        // The readers hold no IntelliJ types at all, so this is a property of the
        // design rather than of a dispatcher: it simply runs anywhere.
        var count = -1
        var failure: Throwable? = null
        val t = Thread {
            try {
                count = CocoCoverage(sites(), profile()).coveredProbeCount()
            } catch (e: Throwable) {
                failure = e
            }
        }
        t.start()
        t.join(30_000)
        assertNull("parse failed off-EDT", failure)
        assertEquals(29, count)
    }

    // --- 2.3.a  the vendored copy has not drifted -----------------------------

    @Test
    fun fixtureMatchesCocoSourceOfTruth() {
        val repo = System.getenv("COCO_REPO")
        if (repo.isNullOrBlank()) return // skipped: no coco checkout to compare against
        for (name in listOf("sites.tsv", "coco.profile")) {
            val origin = File(repo, "fixtures/conformance/$name")
            if (!origin.isFile) continue
            assertEquals(
                "$name has drifted from coco's fixture — regenerate, do not edit the copy",
                origin.readText(),
                fixture(name),
            )
        }
    }

    private fun site(id: Long, kind: CocoSiteKind, line: Int, block: String = "b") = CocoSite(
        id = id, kind = kind, line = line, decision = -1L,
        file = "A.cajeta", owner = "p.A", method = "m()", block = block, target = "",
    )
}
