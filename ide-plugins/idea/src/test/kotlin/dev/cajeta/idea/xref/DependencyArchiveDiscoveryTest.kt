package dev.cajeta.idea.xref

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.nio.file.Files

/**
 * Sub-project dependency discovery (Julian 2026-07-30): with the cajeta-logging
 * REPO opened as the project, its dependency archives never mounted and its
 * imports never resolved — the archives live under `samples/tour/.cajeta/cache/
 * artifacts/`, where that sub-project resolved them, while discovery only ever
 * looked at `<projectBase>/.cajeta/cache/artifacts/`. A repo root is a perfectly
 * ordinary thing to open, so discovery must cover every sub-project.
 */
class DependencyArchiveDiscoveryTest {

    private fun archive(dir: File, name: String) {
        dir.mkdirs()
        File(dir, name).writeText("cja")
    }

    @Test
    fun findsArchivesInSubProjectsAsWellAsTheRoot() {
        val root = Files.createTempDirectory("deps").toFile()
        try {
            archive(File(root, ".cajeta/cache/artifacts"), "aaa.cja")
            File(root, "cajeta.json").writeText("{}")
            archive(File(root, "samples/tour/.cajeta/cache/artifacts"), "bbb.cja")
            File(root, "samples/tour/cajeta.json").writeText("{}")

            val found = CajetaSourceMountGlue.dependencyArchives(root.path)
                .map { it.fileName.toString() }
            assertEquals(listOf("aaa.cja", "bbb.cja"), found.sorted())
        } finally {
            root.deleteRecursively()
        }
    }

    @Test
    fun subProjectArchivesAreFoundWhenTheRootHasNone() {
        // Exactly Julian's layout: the repo root is a library with no resolved
        // dependencies of its own; the consumer sub-project holds them.
        val root = Files.createTempDirectory("deps").toFile()
        try {
            File(root, "cajeta.json").writeText("{}")
            archive(File(root, "samples/tour/.cajeta/cache/artifacts"), "dev.cajeta.logging.cja")
            File(root, "samples/tour/cajeta.json").writeText("{}")

            val found = CajetaSourceMountGlue.dependencyArchives(root.path)
            assertEquals(1, found.size)
            assertTrue(found.single().toString().endsWith("dev.cajeta.logging.cja"))
        } finally {
            root.deleteRecursively()
        }
    }

    @Test
    fun noArchivesAnywhereIsEmptyNotAnError() {
        val root = Files.createTempDirectory("deps").toFile()
        try {
            assertTrue(CajetaSourceMountGlue.dependencyArchives(root.path).isEmpty())
            assertTrue(CajetaSourceMountGlue.dependencyArchives(null).isEmpty())
        } finally {
            root.deleteRecursively()
        }
    }

    @Test
    fun duplicateArchivesAcrossSubProjectsAppearOnce() {
        val root = Files.createTempDirectory("deps").toFile()
        try {
            // the same content-hash archive resolved by two sub-projects
            archive(File(root, "a/.cajeta/cache/artifacts"), "hash.cja")
            File(root, "a/cajeta.json").writeText("{}")
            archive(File(root, "b/.cajeta/cache/artifacts"), "hash.cja")
            File(root, "b/cajeta.json").writeText("{}")

            assertEquals(1, CajetaSourceMountGlue.dependencyArchives(root.path).size)
        } finally {
            root.deleteRecursively()
        }
    }
}
