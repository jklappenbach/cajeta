package dev.cajeta.idea.coverage

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.nio.file.Files

/**
 * ide-coverage-plan Unit 3.2.c — finding the site table that belongs to a
 * profile.
 *
 * The suite points at one file, but reading coverage needs two: probe ids in
 * the profile are positional against `sites.tsv`, so the pair must come from
 * the same run. coco's layout puts them at a fixed offset from each other.
 */
class CocoArtifactsTest {

    private fun tempRun(): java.io.File {
        val base = Files.createTempDirectory("coco-artifacts").toFile()
        Files.createDirectories(base.toPath().resolve("run"))
        return base
    }

    @Test
    fun theSiteTableIsFoundOneLevelUpFromTheProfile() {
        // coco writes <base>/sites.tsv and <base>/run/coco.profile.
        val base = tempRun()
        val sites = java.io.File(base, "sites.tsv").also { it.writeText("coco-sites v1\n") }
        val profile = java.io.File(base, "run/coco.profile").also { it.writeText("coco-profile v1\n") }
        assertEquals(sites, CocoArtifacts.locateSiteTable(profile))
    }

    @Test
    fun aSiteTableBesideTheProfileIsAlsoAccepted() {
        // A hand-assembled pair (the conformance fixture's own layout) should
        // load rather than being rejected on a directory-shape technicality.
        val base = tempRun()
        val sites = java.io.File(base, "run/sites.tsv").also { it.writeText("coco-sites v1\n") }
        val profile = java.io.File(base, "run/coco.profile").also { it.writeText("coco-profile v1\n") }
        assertEquals(sites, CocoArtifacts.locateSiteTable(profile))
    }

    @Test
    fun aSiblingSiteTableWinsOverOneFurtherUp() {
        val base = tempRun()
        java.io.File(base, "sites.tsv").writeText("coco-sites v1\n")
        val near = java.io.File(base, "run/sites.tsv").also { it.writeText("coco-sites v1\n") }
        val profile = java.io.File(base, "run/coco.profile").also { it.writeText("coco-profile v1\n") }
        assertEquals(near, CocoArtifacts.locateSiteTable(profile))
    }

    @Test
    fun noSiteTableMeansNoGuess() {
        // Loading a profile without its table would attribute hits to whatever
        // table turned up. There is no safe fallback, so report nothing found.
        val base = tempRun()
        val profile = java.io.File(base, "run/coco.profile").also { it.writeText("coco-profile v1\n") }
        assertNull(CocoArtifacts.locateSiteTable(profile))
    }

    @Test
    fun theFixturePairLoadsIntoCoverage() {
        val base = Files.createTempDirectory("coco-load").toFile()
        for (name in listOf("sites.tsv", "coco.profile")) {
            java.io.File(base, name).writeText(
                javaClass.getResourceAsStream("/coco/conformance/$name")!!
                    .bufferedReader().readText()
            )
        }
        val c = CocoArtifacts.load(java.io.File(base, "coco.profile"))!!
        assertEquals(47, c.sites.size)
        assertEquals(29, c.coveredProbeCount())
    }
}
