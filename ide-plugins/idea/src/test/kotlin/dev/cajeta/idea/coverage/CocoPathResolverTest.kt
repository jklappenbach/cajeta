package dev.cajeta.idea.coverage

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/**
 * Resolving coco's relative site paths — the step that decides whether anything
 * is painted at all.
 *
 * Written 2026-08-22 against a live failure, not ahead of one. A run loaded
 * cleanly, reported 36.0%, and drew no gutter: every site path had resolved to
 * nothing, `ClassData` was keyed by a relative string, and the platform's
 * annotator silently matched no file. Nothing in the stack treats an
 * unresolvable path as an error, which is exactly why it has to be pinned here.
 *
 * Both halves are asserted, per the habit that keeps catching this class of
 * bug: that resolution SUCCEEDS given the measured root, and that it FAILS
 * given none — a test that only covers the good case cannot tell a working
 * resolver from one that resolves everything to a constant.
 */
class CocoPathResolverTest {

    @get:Rule
    val tmp: TemporaryFolder = TemporaryFolder()

    private fun tourLayout(): Pair<File, File> {
        val root = tmp.newFolder("project")
        val src = File(root, "src/tour/coco").apply { mkdirs() }
        File(src, "Shipping.cajeta").writeText("public class Shipping {}\n")
        return root to File(root, "src")
    }

    @Test
    fun `resolves against the measured source root`() {
        val (root, srcRoot) = tourLayout()
        val profile = File(root, "build/coco/run/coco.merged.profile")
            .apply { parentFile.mkdirs(); writeText("coco-profile v1\n") }

        val resolved = CocoPathResolver.forProfile(profile, listOf(srcRoot))
            .resolve("tour/coco/Shipping.cajeta")

        assertTrue("must be absolute for the annotator to match it", File(resolved).isAbsolute)
        assertEquals(File(srcRoot, "tour/coco/Shipping.cajeta").absolutePath, resolved)
    }

    /**
     * The failure exactly as it occurred: no source roots, because the IDE's
     * module model had none. The output directories the resolver falls back to
     * do not contain the sources, so the path comes back unchanged — and an
     * unchanged relative path is the signal every caller must now treat as "this
     * file will show no coverage".
     */
    @Test
    fun `without the measured root the path does not resolve`() {
        val (root, _) = tourLayout()
        val profile = File(root, "build/coco/run/coco.merged.profile")
            .apply { parentFile.mkdirs(); writeText("coco-profile v1\n") }

        val resolved = CocoPathResolver.forProfile(profile, emptyList())
            .resolve("tour/coco/Shipping.cajeta")

        assertEquals("tour/coco/Shipping.cajeta", resolved)
        assertFalse("an unresolved path must stay relative, never be guessed", File(resolved).isAbsolute)
    }

    /** An absolute path from coco is taken as given, whatever the roots say. */
    @Test
    fun `an absolute site path is kept`() {
        val (root, srcRoot) = tourLayout()
        val profile = File(root, "build/coco/run/coco.profile")
            .apply { parentFile.mkdirs(); writeText("coco-profile v1\n") }
        val absolute = File(srcRoot, "tour/coco/Shipping.cajeta").absolutePath

        assertEquals(
            absolute,
            CocoPathResolver.forProfile(profile, listOf(srcRoot)).resolve(absolute),
        )
    }
}
