package dev.cajeta.idea.xref

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.nio.file.Path

/**
 * lint-own-archive-classpath Unit 2 — archive discovery.
 *
 * The rule every later unit consumes: the project's own `.cja` comes from
 * `cajeta artifact-path`, never from a glob. A build directory accumulates old
 * versions — `cajeta-http/build/archive` holds 0.1.3 and 0.1.4 beside 0.2.0 —
 * and a `head -1` glob picks a stale one (measured 2026-09-13, and it still
 * resolved, which is what makes it dangerous).
 *
 * Everything here is pure: the subprocess and the filesystem are both injected,
 * so none of it needs a compiler or the IntelliJ platform.
 */
class OwnArchiveTest {

    private val declared = Path.of("/p/build/archive/lib-0.2.0.cja")
    private val stale = Path.of("/p/build/archive/lib-0.1.3.cja")

    private fun ok(path: Path): (String) -> Pair<Int, String> =
        { _ -> 0 to "$path\n" }

    private fun onlyExists(vararg present: Path): (Path) -> Boolean =
        { p -> present.contains(p) }

    /** 2.1.1 — the declared path is what discovery returns. */
    @Test
    fun returnsThePathArtifactPathPrints() {
        assertEquals(declared, OwnArchive.resolve(runArtifactPath = ok(declared), exists = onlyExists(declared)))
    }

    /** 2.1.2 — several archives on disk, and the DECLARED one wins. Never the
     *  newest-by-name, never the first a glob would hit. */
    @Test
    fun picksTheDeclaredArchiveNotWhateverElseIsOnDisk() {
        val alsoOnDisk = Path.of("/p/build/archive/lib-0.1.4.cja")
        val got = OwnArchive.resolve(
            runArtifactPath = ok(declared),
            exists = onlyExists(stale, alsoOnDisk, declared),
        )
        assertEquals("the declared archive must win over the others on disk", declared, got)
    }

    /** 2.1.3 — a manifest declaring no artifact exits non-zero. Absent, not an error. */
    @Test
    fun aManifestWithNoArtifactIsAbsent() {
        assertNull(OwnArchive.resolve(runArtifactPath = { _ -> 1 to "no artifact declared\n" }, exists = { true }))
    }

    /** 2.1.4 — artifact-path prints the DECLARED path, which need not exist: it
     *  names where the archive would go, built or not. */
    @Test
    fun aDeclaredButUnbuiltArchiveIsAbsent() {
        assertNull(OwnArchive.resolve(runArtifactPath = ok(declared), exists = { false }))
    }

    /** 2.1.6 — a failing or hanging subprocess yields absent so lint still runs. */
    @Test
    fun aFailingInvocationIsAbsentRatherThanFatal() {
        assertNull(OwnArchive.resolve(runArtifactPath = { _ -> throw RuntimeException("boom") }, exists = { true }))
    }

    /** 2.3.3 — flavor-aware: the first flavor whose archive exists wins, so a
     *  debug-only build is still found when the default flavor is unbuilt. */
    @Test
    fun fallsBackToTheNextFlavorWhenTheFirstIsUnbuilt() {
        val debugArchive = Path.of("/p/build/archive/lib-0.2.0-debug.cja")
        val got = OwnArchive.resolve(
            flavors = listOf("release", "debug"),
            runArtifactPath = { flavor -> 0 to (if (flavor == "release") declared else debugArchive).toString() },
            exists = onlyExists(debugArchive),
        )
        assertEquals(debugArchive, got)
    }

    /** 2.1.5 — the own archive is APPENDED; dependency entries keep their
     *  content and order, so dependency types go on resolving. */
    @Test
    fun theOwnArchiveIsAppendedAndDependenciesAreUntouched() {
        val deps = listOf(Path.of("/p/.cajeta/cache/artifacts/aa.cja"), Path.of("/p/.cajeta/cache/artifacts/bb.cja"))
        assertEquals(deps + declared, OwnArchive.classpath(deps, declared))
    }

    /** 2.1.5 — with no own archive the classpath is exactly the dependencies. */
    @Test
    fun withNoOwnArchiveTheClasspathIsJustTheDependencies() {
        val deps = listOf(Path.of("/p/.cajeta/cache/artifacts/aa.cja"))
        assertEquals(deps, OwnArchive.classpath(deps, null))
    }

    /** The does-not-fire control for 2.1.5: appending must not silently dedupe
     *  or reorder when the own archive is already a dependency entry. */
    @Test
    fun anOwnArchiveAlreadyOnTheDependencyListIsNotDuplicated() {
        val deps = listOf(Path.of("/p/.cajeta/cache/artifacts/aa.cja"), declared)
        assertEquals(deps, OwnArchive.classpath(deps, declared))
    }

    // ── 2.2.2 / 2.3.2 — the cache. A subprocess per keystroke is the thing
    // being avoided, so the tests COUNT resolutions rather than assuming.

    /** 2.3.2 — a warm hit runs no resolution at all. */
    @Test
    fun aWarmCacheDoesNotResolveAgain() {
        var calls = 0
        val cache = OwnArchive.Cache(ttlMs = 1_000, now = { 0L })
        repeat(5) { cache.get("/p", mtimeOf = { 42L }) { calls++; declared } }
        assertEquals("a warm cache must not re-run artifact-path", 1, calls)
    }

    /** 2.2.2 — a rebuilt archive (mtime moved) is re-resolved, so a stale path
     *  is never served after a rebuild. */
    @Test
    fun aChangedArchiveIsResolvedAgain() {
        var calls = 0
        var mtime = 42L
        val cache = OwnArchive.Cache(ttlMs = 1_000, now = { 0L })
        cache.get("/p", mtimeOf = { mtime }) { calls++; declared }
        mtime = 99L
        cache.get("/p", mtimeOf = { mtime }) { calls++; declared }
        assertEquals("a rebuilt archive must invalidate the entry", 2, calls)
    }

    /** Spec §4.4 — an absent archive is re-checked once the TTL lapses, so a
     *  project built mid-session is picked up with no IDE restart. */
    @Test
    fun anAbsentArchiveIsRecheckedAfterTheTtl() {
        var calls = 0
        var clock = 0L
        val cache = OwnArchive.Cache(ttlMs = 1_000, now = { clock })
        cache.get("/p", mtimeOf = { null }) { calls++; null }
        clock = 500L
        cache.get("/p", mtimeOf = { null }) { calls++; null }
        assertEquals("within the TTL the absence is cached", 1, calls)
        clock = 1_500L
        cache.get("/p", mtimeOf = { null }) { calls++; null }
        assertEquals("past the TTL it is re-checked", 2, calls)
    }

    /** Separate projects must not share an entry. */
    @Test
    fun theCacheIsKeyedPerProject() {
        var calls = 0
        val cache = OwnArchive.Cache(ttlMs = 1_000, now = { 0L })
        cache.get("/a", mtimeOf = { 1L }) { calls++; declared }
        cache.get("/b", mtimeOf = { 1L }) { calls++; stale }
        assertEquals(2, calls)
        assertEquals(declared, cache.get("/a", mtimeOf = { 1L }) { calls++; error("must not resolve") })
        assertEquals(stale, cache.get("/b", mtimeOf = { 1L }) { calls++; error("must not resolve") })
    }
}
