package dev.cajeta.idea.lint

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test
import java.nio.file.Files

/**
 * lint-server plan 5.1.b: the identity a staleness check keys on must change
 * when the compiler's CONTENT changes and hold still otherwise. mtime alone is
 * wrong in both directions — `ninja` touching an unchanged output moves it, and
 * so does a relink that produces a byte-identical binary. So the stamp
 * (fileKey + mtime + size) is only a CACHE KEY over the real identity, a
 * content hash: when the stamp moves we rehash, and an unchanged binary comes
 * back with the identity it already had.
 */
class BinaryIdentityCacheTest {

    /** A scriptable file: `stamp` is what stat() would see, `content` the bytes. */
    private class FakeFile(var stamp: String?, var content: String?)

    private class Fixture {
        val files = HashMap<String, FakeFile>()
        var hashes = 0
        val cache = BinaryIdentityCache(
            stampOf = { p -> files[p]?.stamp },
            hashOf = { p -> hashes++; files[p]?.content?.let { "sha256:$it" } },
        )
    }

    @Test
    fun theHashIsComputedOnceWhileTheStampHoldsStill() {
        val f = Fixture()
        f.files["/bin/cajeta"] = FakeFile("ino1:100:360", "aaa")

        val first = f.cache.identityOf("/bin/cajeta")
        repeat(9) { assertEquals(first, f.cache.identityOf("/bin/cajeta")) }

        assertEquals("sha256:aaa", first)
        assertEquals("a still binary must be hashed once, not once per edit", 1, f.hashes)
    }

    // 5.1.b — `ninja` touching an unchanged output moves mtime and nothing else.
    // The stamp moves, so the cache rehashes; the IDENTITY must not move, or the
    // server is restarted for nothing.
    @Test
    fun aTouchedButUnchangedBinaryKeepsItsIdentity() {
        val f = Fixture()
        f.files["/bin/cajeta"] = FakeFile("ino1:100:360", "aaa")
        val before = f.cache.identityOf("/bin/cajeta")

        f.files["/bin/cajeta"]!!.stamp = "ino1:200:360"      // touched: mtime only

        assertEquals(before, f.cache.identityOf("/bin/cajeta"))
        assertEquals("a moved stamp must be re-hashed, not trusted", 2, f.hashes)
    }

    // 5.1.b — a relink that produces a byte-identical binary lands on a new
    // inode with a new mtime. Same bytes, same identity, no restart.
    @Test
    fun anIdenticalRelinkKeepsItsIdentity() {
        val f = Fixture()
        f.files["/bin/cajeta"] = FakeFile("ino1:100:360", "aaa")
        val before = f.cache.identityOf("/bin/cajeta")

        f.files["/bin/cajeta"]!!.stamp = "ino2:200:360"      // new inode + mtime

        assertEquals(before, f.cache.identityOf("/bin/cajeta"))
    }

    @Test
    fun aRebuiltBinaryGetsANewIdentity() {
        val f = Fixture()
        f.files["/bin/cajeta"] = FakeFile("ino1:100:360", "aaa")
        val before = f.cache.identityOf("/bin/cajeta")

        f.files["/bin/cajeta"] = FakeFile("ino2:200:361", "bbb")

        val after = f.cache.identityOf("/bin/cajeta")
        assertEquals("sha256:bbb", after)
        assertEquals(true, before != after)
    }

    // 5.1.c — a deleted binary has no identity. Nothing throws, and the stale
    // reading is not silently reused as if it were current.
    @Test
    fun aDeletedBinaryHasNoIdentityAndIsNotServedFromCache() {
        val f = Fixture()
        f.files["/bin/cajeta"] = FakeFile("ino1:100:360", "aaa")
        assertEquals("sha256:aaa", f.cache.identityOf("/bin/cajeta"))

        f.files["/bin/cajeta"]!!.stamp = null                // deleted

        assertNull(f.cache.identityOf("/bin/cajeta"))
    }

    @Test
    fun anUnhashableButPresentBinaryHasNoIdentity() {
        val f = Fixture()
        f.files["/bin/cajeta"] = FakeFile("ino1:100:360", null)   // stat ok, read fails
        assertNull(f.cache.identityOf("/bin/cajeta"))
    }

    @Test
    fun twoBinariesAreTrackedIndependently() {
        val f = Fixture()
        f.files["/a/cajeta"] = FakeFile("ino1:100:360", "aaa")
        f.files["/b/cajeta"] = FakeFile("ino2:100:360", "bbb")

        assertEquals("sha256:aaa", f.cache.identityOf("/a/cajeta"))
        assertEquals("sha256:bbb", f.cache.identityOf("/b/cajeta"))
        assertEquals("sha256:aaa", f.cache.identityOf("/a/cajeta"))
        assertEquals("one hash per distinct binary", 2, f.hashes)
    }

    // The server writes `sha256:<lowercase hex>` from OpenSSL; this side must
    // spell it identically or every comparison mismatches — and a mismatch does
    // not fail loudly, it latches the whole check off with one log line. Pinned
    // against the published SHA-256 of "abc" so a drift on either side shows up
    // here rather than as a silently disabled guard.
    @Test
    fun theRealHashIsSpelledTheWayTheServerSpellsIt() {
        val f = Files.createTempFile("cajeta-identity", ".bin")
        try {
            Files.write(f, "abc".toByteArray())
            assertEquals(
                "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                BinaryIdentityCache.sha256OfFile(f.toString()))
        } finally {
            Files.deleteIfExists(f)
        }
    }

    @Test
    fun theRealStampMovesWhenTheFileDoesAndIsNullWhenItIsGone() {
        val f = Files.createTempFile("cajeta-stamp", ".bin")
        Files.write(f, "abc".toByteArray())
        val before = BinaryIdentityCache.fileStamp(f.toString())
        assertNotNull(before)

        Files.write(f, "abcd".toByteArray())          // different size
        assertNotEquals(before, BinaryIdentityCache.fileStamp(f.toString()))

        Files.delete(f)
        assertNull(BinaryIdentityCache.fileStamp(f.toString()))
        assertNull(BinaryIdentityCache.sha256OfFile(f.toString()))
    }
}
