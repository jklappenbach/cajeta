package dev.cajeta.idea.xref

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * lint-own-archive-classpath Unit 4 — absent and stale, reported rather than
 * silently degraded.
 *
 * Without the own archive a project's own types cannot resolve, which is exactly
 * the symptom this whole spec exists to fix. An unbuilt project therefore shows
 * the SAME red underlines as the original bug, and the developer has no way to
 * tell "not built yet" from "the fix does not work" — that ambiguity cost real
 * time on 2026-09-13. The state is reported instead.
 *
 * Pure: mtimes are passed in, so nothing here touches a filesystem or a clock.
 */
class ArchiveHealthTest {

    /** 4.1.5 — an archive older than the newest source is STALE, and still used
     *  for resolution by the caller (spec §5.1). */
    @Test
    fun anArchiveOlderThanItsSourcesIsStale() {
        assertEquals(ArchiveHealth.State.STALE, ArchiveHealth.assess(archiveMtime = 100, newestSourceMtime = 200))
    }

    /** 4.1.6 — a current archive is healthy and says nothing. */
    @Test
    fun aCurrentArchiveIsOk() {
        assertEquals(ArchiveHealth.State.OK, ArchiveHealth.assess(archiveMtime = 300, newestSourceMtime = 200))
        assertNull("a healthy archive must not produce a notice", ArchiveHealth.reason(ArchiveHealth.State.OK))
    }

    /** An archive exactly as old as the newest source is not stale — equal
     *  timestamps are the common case right after a build. */
    @Test
    fun anArchiveAsNewAsItsSourcesIsOk() {
        assertEquals(ArchiveHealth.State.OK, ArchiveHealth.assess(archiveMtime = 200, newestSourceMtime = 200))
    }

    /** 4.1.1 / 4.1.2 — no archive is ABSENT, and the reason names the remedy.
     *  It must not read as a defect in the edited file (spec §4.3). */
    @Test
    fun noArchiveIsAbsentAndTheReasonNamesTheRemedy() {
        assertEquals(ArchiveHealth.State.ABSENT, ArchiveHealth.assess(archiveMtime = null, newestSourceMtime = 200))
        val why = ArchiveHealth.reason(ArchiveHealth.State.ABSENT)!!
        assertTrue("must say the project is not built: $why", why.contains("not built"))
        assertTrue("must name the remedy: $why", why.contains("cajeta build"))
        assertTrue("must say own types cannot resolve: $why", why.contains("own types"))
    }

    /** 4.1.5 — the stale reason names its remedy too, and says resolution still
     *  works so it does not read as a failure. */
    @Test
    fun theStaleReasonNamesItsRemedy() {
        val why = ArchiveHealth.reason(ArchiveHealth.State.STALE)!!
        assertTrue("must name the remedy: $why", why.contains("rebuild"))
    }

    /** 4.1.7 — staleness is judged by MTIME, never by version string. A 0.1.3
     *  archive resolved a 0.2.0 tree correctly on 2026-09-13 because the types
     *  had not moved, so a version mismatch is not by itself a defect. The API
     *  takes no version at all, which is the point: this asserts an old-looking
     *  archive that is newer than the sources is healthy. */
    @Test
    fun stalenessIgnoresTheVersionString() {
        assertEquals(
            "an archive named 0.1.3 but newer than every source is NOT stale",
            ArchiveHealth.State.OK,
            ArchiveHealth.assess(archiveMtime = 500, newestSourceMtime = 100),
        )
    }

    /** When the source mtime cannot be determined, staleness is unknowable and
     *  the archive is used without a warning — a guess would cry wolf. */
    @Test
    fun anUnknownSourceMtimeIsNotStale() {
        assertEquals(ArchiveHealth.State.OK, ArchiveHealth.assess(archiveMtime = 100, newestSourceMtime = null))
    }

    /** The does-not-fire control that matters most in practice: a project that
     *  declares NO artifact is silent. "Declared but unbuilt" and "declares
     *  nothing" both have no archive, and only the first is worth a notice —
     *  otherwise every ordinary application project carries a permanent false
     *  "not built" warning, which is how notices get trained away. */
    @Test
    fun aProjectDeclaringNoArtifactIsSilent() {
        assertEquals(
            ArchiveHealth.State.OK,
            ArchiveHealth.assess(archiveMtime = null, newestSourceMtime = 200, declared = false),
        )
        assertEquals(
            "...while a DECLARED but unbuilt artifact still reports",
            ArchiveHealth.State.ABSENT,
            ArchiveHealth.assess(archiveMtime = null, newestSourceMtime = 200, declared = true),
        )
    }
}
