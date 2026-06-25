package dev.cajeta.idea.buildtool

import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File

/**
 * W-buildtool unit 4: the process layer. The bad-path surfacing is deterministic
 * (no spawn); the discovery happy-path is an integration test against the real
 * `cajeta` binary, skipped (assumeTrue) when the binary isn't built — so CI on a
 * host without it stays green (§15.3, plan 4.1.2).
 */
class CajetaBuildRunnerTest {

    // Repo root = two levels up from the gradle test working dir (ide-plugins/idea).
    private val repoRoot: File =
        File(System.getProperty("user.dir")).parentFile.parentFile

    private fun buildToolOrNull(): File? {
        System.getenv("CAJETA_BUILD_TOOL")?.let { env ->
            File(env).takeIf { it.canExecute() }?.let { return it }
        }
        return File(repoRoot, "build/src/cajeta").takeIf { it.canExecute() }
    }

    @Test
    fun invalidBuildToolPathFailsLoudlyWithoutSpawn() {
        val r = CajetaBuildRunner.discover("/no/such/cajeta-xyz", "/whatever/cajeta.json")
        assertTrue(r is CajetaBuildRunner.DiscoverResult.Failed)
        assertTrue(
            "failure should name the path problem",
            (r as CajetaBuildRunner.DiscoverResult.Failed).reason.contains("build tool path"),
        )
    }

    @Test
    fun discoversTasksFromRealBinary() {
        val tool = buildToolOrNull()
        assumeTrue("cajeta build tool not built; skipping integration", tool != null)
        val manifest = File(repoRoot, "samples/profile/cajeta.json")
        assumeTrue("sample manifest missing; skipping", manifest.exists())

        val r = CajetaBuildRunner.discover(tool!!.path, manifest.path)
        assertTrue(
            "expected discovery to succeed, got $r",
            r is CajetaBuildRunner.DiscoverResult.Ok,
        )
        val model = (r as CajetaBuildRunner.DiscoverResult.Ok).model
        assertTrue("manifest path should be absolute", model.manifest.endsWith("cajeta.json"))
        assertTrue("expected at least one task", model.tasks.isNotEmpty())
        assertTrue(
            "expected a 'build' task in the profile sample",
            model.tasks.any { it.name == "build" },
        )
        // builtins always present from the contract.
        assertTrue(model.builtins.any { it.name == "tasks" })
    }

    @Test
    fun nonexistentManifestFailsGracefullyAgainstRealBinary() {
        val tool = buildToolOrNull()
        assumeTrue("cajeta build tool not built; skipping integration", tool != null)
        val r = CajetaBuildRunner.discover(tool!!.path, "/no/such/dir/cajeta.json")
        // Non-zero exit -> Failed, never a throw / hang.
        assertTrue(r is CajetaBuildRunner.DiscoverResult.Failed)
    }
}
