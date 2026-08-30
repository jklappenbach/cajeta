package dev.cajeta.idea.lint

import java.io.FileInputStream
import java.nio.file.Files
import java.nio.file.Paths
import java.nio.file.attribute.BasicFileAttributes
import java.security.MessageDigest

/**
 * The content identity of a compiler binary, with the expensive part behind a
 * cheap stamp (lint-server-spec §2.8, plan 5.1.b).
 *
 * A staleness check needs an identity that moves when the compiler's BEHAVIOUR
 * moves and holds still otherwise. mtime fails in both directions: `ninja`
 * touching an unchanged output moves it, and a relink producing byte-identical
 * output moves it too — either would restart a healthy daemon and pay a full
 * stdlib prime for nothing. So the identity is a content hash.
 *
 * Hashing 360 MB on every keystroke is not an option, so the stamp — file key
 * (inode where the platform exposes one), mtime, size — is used purely as a
 * CACHE KEY: while it holds still the content cannot have changed, and the
 * cached hash stands. When it moves we rehash, which is once per build.
 *
 * The stat and the hash are injected so the policy is testable without a
 * filesystem; [real] binds the platform ones.
 */
class BinaryIdentityCache(
    private val stampOf: (String) -> String?,
    private val hashOf: (String) -> String?,
) {
    private data class Entry(val stamp: String, val identity: String?)

    private val cache = HashMap<String, Entry>()

    /**
     * The identity of the file at [path], or null when it cannot be read — a
     * missing binary has no identity, and the last one it had is not a stand-in
     * for the current one.
     */
    @Synchronized
    fun identityOf(path: String): String? {
        val stamp = stampOf(path) ?: return null
        cache[path]?.let { if (it.stamp == stamp) return it.identity }
        val identity = hashOf(path)
        cache[path] = Entry(stamp, identity)
        return identity
    }

    companion object {
        fun real(): BinaryIdentityCache = BinaryIdentityCache(::fileStamp, ::sha256OfFile)

        /**
         * File key + mtime + size. `fileKey` is null on Windows, where the stamp
         * degrades to mtime + size — weaker, but it is only a cache key, so the
         * cost of a weak one is an extra hash, never a wrong answer.
         */
        fun fileStamp(path: String): String? = try {
            val attrs = Files.readAttributes(Paths.get(path), BasicFileAttributes::class.java)
            "${attrs.fileKey()}:${attrs.lastModifiedTime().toMillis()}:${attrs.size()}"
        } catch (_: Exception) {
            null
        }

        /** `sha256:<hex>` — the same spelling the server writes on the wire. */
        fun sha256OfFile(path: String): String? = try {
            val digest = MessageDigest.getInstance("SHA-256")
            FileInputStream(path).use { input ->
                val buf = ByteArray(1 shl 16)
                while (true) {
                    val n = input.read(buf)
                    if (n <= 0) break
                    digest.update(buf, 0, n)
                }
            }
            digest.digest().joinToString("", prefix = "sha256:") { "%02x".format(it) }
        } catch (_: Exception) {
            null
        }
    }
}
