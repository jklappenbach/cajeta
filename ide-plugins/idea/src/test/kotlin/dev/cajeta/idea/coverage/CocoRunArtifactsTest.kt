package dev.cajeta.idea.coverage

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.nio.file.Files

/**
 * ide-coverage-plan Unit 4.1.d / 4.1.e — finding a completed run's artifacts,
 * and the retention that lets results be re-examined without re-running.
 */
class CocoRunArtifactsTest {

    private fun outDir(): File {
        val base = Files.createTempDirectory("coco-out").toFile()
        File(base, "run").mkdirs()
        File(base, "sites.tsv").writeText("coco-sites v1\n")
        return base
    }

    @Test
    fun theWholeRunProfileIsFoundUnderRun() {
        val base = outDir()
        val p = File(base, "run/coco.profile").also { it.writeText("coco-profile v1\n") }
        assertEquals(p, CocoArtifacts.discoverProfile(base))
    }

    @Test
    fun theAttributionMergedProfileWinsWhenPresent() {
        // coco writes coco.merged.profile when the run tracked per-test data,
        // and its own report path prefers it. Preferring the plain one would
        // silently discard the attribution the run paid for.
        val base = outDir()
        File(base, "run/coco.profile").writeText("coco-profile v1\n")
        val merged = File(base, "run/coco.merged.profile").also { it.writeText("coco-profile v1\n") }
        assertEquals(merged, CocoArtifacts.discoverProfile(base))
    }

    @Test
    fun perTestProfilesAreNotMistakenForTheRunProfile() {
        // coco-test-<name>.profile files sit beside the run profile. Picking one
        // would report a single test's coverage as the whole run's.
        val base = outDir()
        File(base, "run/coco-test-Alpha.profile").writeText("coco-profile v1\n")
        assertNull("no whole-run profile yet", CocoArtifacts.discoverProfile(base))
    }

    @Test
    fun anOutDirectoryWithNoRunYieldsNothing() {
        assertNull(CocoArtifacts.discoverProfile(outDir()))
        assertNull(CocoArtifacts.discoverProfile(File("/nonexistent-coco-out")))
    }

    // --- 4.1.e  retention -----------------------------------------------------

    @Test
    fun artifactsSurviveLoadingSoASecondLoadNeedsNoRun() {
        // 4.1.e / 4.3.b: loading must not consume or move the artifacts. If it
        // did, re-rendering would silently require another run.
        val base = Files.createTempDirectory("coco-retain").toFile()
        File(base, "run").mkdirs()
        for ((rel, res) in listOf("sites.tsv" to "sites.tsv", "run/coco.profile" to "coco.profile")) {
            File(base, rel).writeText(
                javaClass.getResourceAsStream("/coco/conformance/$res")!!.bufferedReader().readText()
            )
        }
        val profile = CocoArtifacts.discoverProfile(base)!!
        val first = CocoArtifacts.load(profile)!!
        assertTrue(profile.isFile)
        val second = CocoArtifacts.load(profile)!!
        assertEquals(first.sites.size, second.sites.size)
        assertEquals(first.coveredProbeCount(), second.coveredProbeCount())
    }

    @Test
    fun theSiteTableIsFoundFromADiscoveredProfileOneLevelUp() {
        // discoverProfile returns <out>/run/coco.profile; locateSiteTable must
        // then find <out>/sites.tsv. The two halves have to agree on the layout.
        val base = outDir()
        File(base, "run/coco.profile").writeText("coco-profile v1\n")
        val profile = CocoArtifacts.discoverProfile(base)!!
        assertEquals(File(base, "sites.tsv"), CocoArtifacts.locateSiteTable(profile))
    }
}
