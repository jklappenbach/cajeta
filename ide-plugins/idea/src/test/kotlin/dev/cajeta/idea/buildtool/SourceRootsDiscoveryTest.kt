package dev.cajeta.idea.buildtool

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/**
 * lint-own-archive-classpath Unit 7 — discover EVERY source root, not just the
 * conventional one.
 *
 * The export previously visited `conventionalSourceRoot(base)` = `src/main/cajeta`
 * only, so a file in a separate test root was absent from the index entirely and
 * every import in it was dead. Measured on cajeta-http: the shard named
 * `HttpSerializer` 587 times and `ServerTests` 0.
 *
 * Roots are DERIVED — each file's declared `package a.b.c` is stripped from the
 * tail of its path — rather than matched against a list of conventional paths.
 * A candidate list was rejected because no convention exists: surveyed
 * 2026-09-13, cajeta-http uses `test/src` while cajeta-codec and cajeta-logging
 * use `src/test/cajeta`, and no manifest declares a test root. 7.1.1 and 7.1.2
 * are those two real layouts, and one rule must satisfy both.
 */
class SourceRootsDiscoveryTest {

    @get:Rule
    val tmp = TemporaryFolder()

    private fun write(rel: String, body: String) {
        val f = File(tmp.root, rel)
        f.parentFile.mkdirs()
        f.writeText(body)
    }

    private fun roots(): List<String> =
        CajetaRoots.sourceRootsOf(tmp.root.path).map {
            it.removePrefix(tmp.root.path).removePrefix(File.separator)
        }.sorted()

    /** 7.1.1 — the cajeta-http layout: library and tests in unrelated trees. */
    @Test
    fun findsBothRootsOfTheCajetaHttpLayout() {
        write("src/main/cajeta/dev/cajeta/http/HttpRequest.cajeta", "package dev.cajeta.http;\nclass HttpRequest {}")
        write("test/src/dev/cajeta/http/test/ServerTests.cajeta", "package dev.cajeta.http.test;\nclass ServerTests {}")
        assertEquals(listOf("src/main/cajeta", "test/src"), roots())
    }

    /** 7.1.2 — the cajeta-codec / cajeta-logging layout. A DIFFERENT shape, and
     *  the same rule must find it, which is the whole argument against a
     *  hardcoded candidate list. */
    @Test
    fun findsBothRootsOfTheCodecLayout() {
        write("src/main/cajeta/dev/cajeta/codec/Gzip.cajeta", "package dev.cajeta.codec;\nclass Gzip {}")
        write("src/test/cajeta/dev/cajeta/codec/test/GzipTests.cajeta", "package dev.cajeta.codec.test;\nclass GzipTests {}")
        assertEquals(listOf("src/main/cajeta", "src/test/cajeta"), roots())
    }

    /** 7.1.3 — generated and vendored trees are not roots. A stale copy of the
     *  sources under build/ would otherwise be discovered and exported. */
    @Test
    fun excludesGeneratedAndVendoredTrees() {
        write("src/main/cajeta/p/A.cajeta", "package p;\nclass A {}")
        write("build/archive/extracted/p/A.cajeta", "package p;\nclass A {}")
        write("tmp/scratch/p/B.cajeta", "package p;\nclass B {}")
        write(".cajeta/cache/sources/p/C.cajeta", "package p;\nclass C {}")
        assertEquals(listOf("src/main/cajeta"), roots())
    }

    /** 7.1.4 — no package declaration: the file's own directory is the root. */
    @Test
    fun aFileWithNoPackageContributesItsOwnDirectory() {
        write("loose/Scratch.cajeta", "class Scratch {}")
        assertEquals(listOf("loose"), roots())
    }

    /** 7.1.5 — a package that does not match the on-disk layout falls back to the
     *  file's directory rather than truncating to a wrong ancestor. */
    @Test
    fun aMismatchedLayoutContributesItsOwnDirectory() {
        write("odd/Thing.cajeta", "package totally.different.path;\nclass Thing {}")
        assertEquals(listOf("odd"), roots())
    }

    /** 7.1.6 — the does-not-fire control. A single-root project yields exactly
     *  one root, so nothing changes for projects that never had this problem. */
    @Test
    fun aSingleRootProjectYieldsExactlyOneRoot() {
        write("src/main/cajeta/p/A.cajeta", "package p;\nclass A {}")
        write("src/main/cajeta/p/q/B.cajeta", "package p.q;\nclass B {}")
        assertEquals(listOf("src/main/cajeta"), roots())
    }

    /** A project with no sources at all discovers nothing, and does not throw. */
    @Test
    fun anEmptyProjectYieldsNoRoots() {
        assertTrue(CajetaRoots.sourceRootsOf(tmp.root.path).isEmpty())
    }

    /** 7.1.7 — two roots write DISJOINT shards, so passes accumulate and no merge
     *  step is needed. Shards are keyed by source-relative path. */
    @Test
    fun filesFromDifferentRootsGetDifferentShards() {
        val a = dev.cajeta.idea.xref.CajetaXrefShards.shardName("src/main/cajeta/dev/cajeta/http/HttpRequest.cajeta")
        val b = dev.cajeta.idea.xref.CajetaXrefShards.shardName("test/src/dev/cajeta/http/test/ServerTests.cajeta")
        assertTrue("shard names must differ or one pass would clobber the other", a != b)
    }
}
