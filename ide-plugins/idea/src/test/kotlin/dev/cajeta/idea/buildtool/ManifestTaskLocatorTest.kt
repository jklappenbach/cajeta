package dev.cajeta.idea.buildtool

import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * W-buildtool unit 5.2.3 core: locate a task's declaration offset in cajeta.json
 * text for "Open in manifest" navigation.
 */
class ManifestTaskLocatorTest {

    private val manifest = """
        {
          "name": "demo",
          "dependencies": { "build": "1.0" },
          "tasks": {
            "build": { "description": "Compile" },
            "clean": { "description": "Wipe" }
          }
        }
    """.trimIndent()

    @Test
    fun findsTaskInsideTheTasksBlock() {
        val off = ManifestTaskLocator.offsetOf(manifest, "clean")
        assertTrue("clean should be found", off != null)
        // The located offset should be the task key inside the tasks block,
        // i.e. the substring there starts with "clean".
        assertTrue(manifest.substring(off!!).startsWith("\"clean\""))
    }

    @Test
    fun prefersTaskKeyOverASameNamedKeyBeforeTheTasksBlock() {
        // "build" also appears as a dependency key earlier; navigation must land
        // on the one inside "tasks", not the dependency.
        val off = ManifestTaskLocator.offsetOf(manifest, "build")!!
        val tasksAt = manifest.indexOf("\"tasks\"")
        assertTrue("located build must be after the tasks block opens", off > tasksAt)
    }

    @Test
    fun returnsNullForUnknownTaskOrMissingBlock() {
        assertNull(ManifestTaskLocator.offsetOf(manifest, "nope"))
        assertNull(ManifestTaskLocator.offsetOf("""{ "name": "x" }""", "build"))
    }
}
