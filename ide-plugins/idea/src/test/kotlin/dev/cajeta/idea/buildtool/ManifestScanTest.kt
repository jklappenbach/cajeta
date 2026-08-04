package dev.cajeta.idea.buildtool

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.nio.file.Files

/**
 * Manifest discovery (Julian 2026-07-30: a run config created on a project
 * whose manifest sits in a subdirectory — cajeta-logging/samples/tour — offered
 * no entry methods). The root manifest is only one of the project's manifests;
 * a bounded scan finds nested ones without walking build output.
 */
class ManifestScanTest {

    @Test
    fun skipsBuildOutputAndDotDirectories() {
        assertTrue(ManifestScan.shouldDescend("samples"))
        assertTrue(ManifestScan.shouldDescend("src"))
        assertFalse(ManifestScan.shouldDescend("build"))
        assertFalse(ManifestScan.shouldDescend(".cajeta"))
        assertFalse(ManifestScan.shouldDescend(".git"))
        assertFalse(ManifestScan.shouldDescend("node_modules"))
    }

    @Test
    fun findsRootAndNestedManifestsWithinDepth() {
        val root = Files.createTempDirectory("mscan").toFile()
        try {
            File(root, "cajeta.json").writeText("{}")
            File(root, "samples/tour").mkdirs()
            File(root, "samples/tour/cajeta.json").writeText("{}")
            // too deep for the default budget
            File(root, "a/b/c/d").mkdirs()
            File(root, "a/b/c/d/cajeta.json").writeText("{}")
            // must not be walked into
            File(root, "build/exe").mkdirs()
            File(root, "build/exe/cajeta.json").writeText("{}")

            val found = ManifestScan.findManifests(root, maxDepth = 3)
                .map { it.absolutePath.removePrefix(root.absolutePath) }
                .sorted()

            assertEquals(listOf("/cajeta.json", "/samples/tour/cajeta.json"), found)
        } finally {
            root.deleteRecursively()
        }
    }

    @Test
    fun missingRootIsEmptyNotAnError() {
        assertEquals(emptyList<File>(), ManifestScan.findManifests(File("/no/such/dir")))
    }
}
