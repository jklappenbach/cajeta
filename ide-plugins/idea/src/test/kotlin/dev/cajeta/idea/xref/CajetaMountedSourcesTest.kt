package dev.cajeta.idea.xref

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.file.Files
import java.nio.file.Path

/**
 * ide-symbol-index Unit 8 (plan 8.1.1 - 8.1.8): library and stdlib source
 * mounting. Every dependency `.cja` already embeds its full source
 * (ClassSource entries); the stdlib comes from `cajeta stdlib extract`
 * (Unit 4). The local path is complete, offline, and guaranteed to be the
 * EXACT source the artifact was built from — GitHub is a fallback, not the
 * mechanism (spec §6). Extraction is injectable here so these tests exercise
 * the cache/read-only/decision logic without the compiler binary or network.
 */
class CajetaMountedSourcesTest {

    private fun tempRoot(): Path =
        Files.createTempDirectory("cajeta-mount-test")

    /** A fake .cja: content only matters for hashing. */
    private fun fakeCja(dir: Path, bytes: String): Path {
        val f = dir.resolve("dep.cja")
        Files.write(f, bytes.toByteArray())
        return f
    }

    /** A fake extractor that writes [files] and reports success. */
    private fun extractorWriting(vararg files: String): Pair<(Path, Path) -> Int, () -> Int> {
        var calls = 0
        val fn = { _: Path, dest: Path ->
            calls++
            for (rel in files) {
                val p = dest.resolve(rel)
                Files.createDirectories(p.parent)
                Files.write(p, "class X {}".toByteArray())
            }
            files.size
        }
        return fn to { calls }
    }

    // ---- 8.1.1 / 8.1.2 — hash-keyed extraction, cached -------------------------

    @Test
    fun extractionIsKeyedByContentHashAndCached() {
        val root = tempRoot()
        val cja = fakeCja(root, "archive-bytes-v1")
        val (extract, calls) = extractorWriting("dev/cajeta/codec/Avro.cajeta")

        val mounted = CajetaMountedSources.mountArchive(cja, root.resolve("cache"), extract)
        assertNotNull(mounted)
        assertTrue(Files.exists(mounted!!.resolve("dev/cajeta/codec/Avro.cajeta")))
        assertEquals(1, calls())

        // 8.1.2 — second request hits the cache; the extractor never re-runs.
        val again = CajetaMountedSources.mountArchive(cja, root.resolve("cache"), extract)
        assertEquals(mounted, again)
        assertEquals(1, calls())

        // Different content → different key → separate mount.
        val other = fakeCja(root.resolve("o").also { Files.createDirectories(it) },
            "archive-bytes-v2")
        CajetaMountedSources.mountArchive(other, root.resolve("cache"), extract)
        assertEquals(2, calls())
    }

    // ---- 8.1.3 — read-only ------------------------------------------------------

    @Test
    fun extractedSourcesAreReadOnly() {
        val root = tempRoot()
        val cja = fakeCja(root, "ro-bytes")
        val (extract, _) = extractorWriting("a/B.cajeta")

        val mounted = CajetaMountedSources.mountArchive(cja, root.resolve("cache"), extract)!!
        assertFalse("mounted source must be read-only",
            Files.isWritable(mounted.resolve("a/B.cajeta")))
    }

    // ---- 8.1.4 — stdlib mount keyed on the compiler identity marker ---------------

    @Test
    fun stdlibMountIsKeyedOnTheIdentityMarkerAndReadOnly() {
        val root = tempRoot()
        var calls = 0
        // Fake `cajeta stdlib extract <dest>`: writes tree + identity marker,
        // exactly what the Unit 4 subcommand produces.
        val extract = { dest: Path ->
            calls++
            val f = dest.resolve("cajeta/lang/Object.cajeta")
            Files.createDirectories(f.parent)
            Files.write(f, "class Object {}".toByteArray())
            Files.write(dest.resolve(".cajeta-stdlib.json"),
                """{"version": "1.2.3", "gitHash": "abc", "fileCount": 1}""".toByteArray())
            0
        }

        val m1 = CajetaMountedSources.mountStdlib("1.2.3-abc", root.resolve("cache"), extract)
        assertNotNull(m1)
        assertEquals(1, calls)
        assertFalse(Files.isWritable(m1!!.resolve("cajeta/lang/Object.cajeta")))

        // Same compiler identity → cache hit.
        CajetaMountedSources.mountStdlib("1.2.3-abc", root.resolve("cache"), extract)
        assertEquals(1, calls)

        // New compiler → re-extract (the plugin keys its cache on the marker).
        CajetaMountedSources.mountStdlib("1.2.4-def", root.resolve("cache"), extract)
        assertEquals(2, calls)
    }

    // ---- 8.1.6 / 8.1.7 / 8.1.8 — the GitHub fallback decision ---------------------

    @Test
    fun sourcelessDependencyOffersTheFetchInsteadOfSilence() {
        val root = tempRoot()
        val cja = fakeCja(root, "no-source-bytes")
        val emptyExtractor = { _: Path, _: Path -> 0 }   // no ClassSource entries

        val outcome = LibrarySourceResolver.resolve(
            cja, "https://github.com/x/y", root.resolve("cache"),
            extractor = emptyExtractor,
            fetcher = { _, _ -> error("must not fetch without opt-in") })
        assertTrue(outcome is LibrarySourceResolver.Outcome.OfferFetch)
        assertEquals("https://github.com/x/y",
            (outcome as LibrarySourceResolver.Outcome.OfferFetch).repoUrl)
    }

    @Test
    fun fetchedSourceIsFlaggedUnverified() {
        val root = tempRoot()
        val cja = fakeCja(root, "no-source-2")
        val fetched = root.resolve("fetched")
        Files.createDirectories(fetched)
        Files.write(fetched.resolve("Y.cajeta"), "class Y {}".toByteArray())

        val outcome = LibrarySourceResolver.resolve(
            cja, "https://github.com/x/y", root.resolve("cache"),
            extractor = { _, _ -> 0 },
            fetcher = { _, _ -> fetched },
            optInFetch = true)
        assertTrue(outcome is LibrarySourceResolver.Outcome.Mounted)
        val m = outcome as LibrarySourceResolver.Outcome.Mounted
        assertTrue("GitHub-fetched source must be marked unverified (§6.0.6)",
            m.unverified)
    }

    @Test
    fun aSourcedDependencyNeverTouchesTheNetwork() {
        val root = tempRoot()
        val cja = fakeCja(root, "sourced-bytes")
        val (extract, _) = extractorWriting("a/Z.cajeta")
        var fetches = 0

        val outcome = LibrarySourceResolver.resolve(
            cja, "https://github.com/x/y", root.resolve("cache"),
            extractor = extract,
            fetcher = { _, _ -> fetches++; tempRoot() },
            optInFetch = true)
        assertTrue(outcome is LibrarySourceResolver.Outcome.Mounted)
        assertFalse((outcome as LibrarySourceResolver.Outcome.Mounted).unverified)
        assertEquals("8.1.8: with source in the archive, no network call", 0, fetches)
    }

    // ---- 8.2.4 — the debugger's mounted-source lookup ------------------------------

    @Test
    fun frameSourcePathsResolveIntoMountedRootsBySuffix() {
        val root = tempRoot()
        val cja = fakeCja(root, "dbg-bytes")
        val (extract, _) = extractorWriting("cajeta/collection/ArrayList.cajeta")
        val mounted = CajetaMountedSources.mountArchive(cja, root.resolve("cache"), extract)!!
        CajetaMountedSources.registerForTests(mounted)
        try {
            // A stdlib frame reports the root-relative path the compiler knew.
            assertNotNull(CajetaMountedSources
                .findMountedNioBySuffix("cajeta/collection/ArrayList.cajeta"))
            // A compile-machine absolute path still finds the mounted twin.
            assertNotNull(CajetaMountedSources
                .findMountedNioBySuffix("/build/box/runtime/src/cajeta/collection/ArrayList.cajeta"))
            assertNull(CajetaMountedSources.findMountedNioBySuffix("no/Such.cajeta"))
        } finally {
            CajetaMountedSources.unregisterForTests(mounted)
        }
    }
}
