package dev.cajeta.idea.xref

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ide-symbol-index 8.3 first-open fix (Julian, 2026-07-30): dependency source
 * mounting must re-run when resolved archives LAND in
 * `<base>/.cajeta/cache/artifacts/` — the startup-only mount runs before a
 * fresh consumer project's first build has resolved anything. This predicate
 * decides which VFS events matter; pure.
 */
class CajetaArtifactsWatchTest {

    private val base = "/home/dev/proj"

    @Test
    fun archiveInTheProjectsArtifactsDirMatches() {
        assertTrue(CajetaArtifactsWatch.isArtifactArchivePath(
            "$base/.cajeta/cache/artifacts/f27467c6.cja", base))
    }

    @Test
    fun otherPathsDoNotMatch() {
        // an archive elsewhere (downloads, another project, source tree)
        assertFalse(CajetaArtifactsWatch.isArtifactArchivePath(
            "$base/.cajeta/cache/downloads/dev.cajeta.logging-0.6.0.cja", base))
        assertFalse(CajetaArtifactsWatch.isArtifactArchivePath(
            "/home/dev/other/.cajeta/cache/artifacts/x.cja", base))
        // a non-archive file in the right dir
        assertFalse(CajetaArtifactsWatch.isArtifactArchivePath(
            "$base/.cajeta/cache/artifacts/notes.txt", base))
        // null base = no project to match against
        assertFalse(CajetaArtifactsWatch.isArtifactArchivePath(
            "$base/.cajeta/cache/artifacts/f.cja", null))
    }

    @Test
    fun trailingSlashOnBaseIsTolerated() {
        assertTrue(CajetaArtifactsWatch.isArtifactArchivePath(
            "$base/.cajeta/cache/artifacts/f.cja", "$base/"))
    }
}
