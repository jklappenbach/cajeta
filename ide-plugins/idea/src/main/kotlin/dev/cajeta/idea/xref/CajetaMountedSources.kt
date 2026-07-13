package dev.cajeta.idea.xref

import com.intellij.openapi.diagnostic.Logger
import com.intellij.openapi.vfs.LocalFileSystem
import com.intellij.openapi.vfs.VirtualFile
import java.nio.file.Files
import java.nio.file.Path
import java.security.MessageDigest
import java.util.concurrent.CopyOnWriteArraySet

/**
 * Library and stdlib source mounting (ide-symbol-index Unit 8, spec §6).
 *
 * Every dependency `.cja` embeds its full `.cajeta` source (ClassSource
 * entries — `cajeta archive extract` gets them out), and the stdlib source
 * comes from `cajeta stdlib extract` (Unit 4). The local path is complete,
 * offline, and guaranteed to be the EXACT source the artifact was built
 * from; GitHub is a fallback, not the mechanism.
 *
 * Extraction functions are injected so cache/read-only/decision logic tests
 * run without the compiler binary; production wiring passes real
 * subprocess runners.
 */
object CajetaMountedSources {

    private val log = Logger.getInstance(CajetaMountedSources::class.java)

    /** Mounted roots, for the debugger's frame-path lookup (8.2.4). */
    private val mountedRoots = CopyOnWriteArraySet<Path>()

    /**
     * Mount a dependency archive's embedded source, keyed by the archive's
     * CONTENT hash (8.1.1) — dependencies are immutable, so this is a
     * once-per-version cost (spec §6.0.2). Returns null when the archive
     * carries no ClassSource entries (the caller offers the GitHub
     * fallback, 8.1.6). [extractor] returns the number of files extracted.
     */
    fun mountArchive(cja: Path, cacheRoot: Path,
                     extractor: (Path, Path) -> Int): Path? {
        val dest = cacheRoot.resolve(sha256(cja))
        val marker = dest.resolve(".mounted.json")
        if (Files.exists(marker)) {           // 8.1.2 — cache hit
            mountedRoots.add(dest)
            return dest
        }
        Files.createDirectories(dest)
        val count = try {
            extractor(cja, dest)
        } catch (e: Exception) {
            log.warn("source extraction failed for $cja: ${e.message}")
            return null
        }
        if (count <= 0) return null
        sealReadOnly(dest)                    // 8.1.3
        Files.write(marker, """{"files": $count, "unverified": false}"""
            .toByteArray())
        mountedRoots.add(dest)
        return dest
    }

    /**
     * Mount the stdlib via the Unit 4 subcommand, keyed on the compiler
     * identity (the `.cajeta-stdlib.json` marker's version+gitHash — 8.2.3):
     * a new compiler re-extracts, the same one hits the cache.
     */
    fun mountStdlib(compilerIdentity: String, cacheRoot: Path,
                    extractor: (Path) -> Int): Path? {
        val key = compilerIdentity.replace(Regex("[^A-Za-z0-9._-]"), "_")
        val dest = cacheRoot.resolve("stdlib-$key")
        val marker = dest.resolve(".cajeta-stdlib.json")
        if (Files.exists(marker)) {
            mountedRoots.add(dest)
            return dest
        }
        Files.createDirectories(dest)
        val rc = try {
            extractor(dest)
        } catch (e: Exception) {
            log.warn("stdlib extraction failed: ${e.message}")
            return null
        }
        if (rc != 0 || !Files.exists(marker)) return null
        sealReadOnly(dest)
        mountedRoots.add(dest)
        return dest
    }

    /**
     * The debugger hook (8.2.4): a library/stdlib frame reports the path the
     * COMPILER knew — root-relative ("cajeta/lang/String.cajeta") or absolute
     * on the machine that built the artifact. Neither exists locally; the
     * mounted twin does. Longest-suffix match into the mounted roots.
     */
    fun findMountedBySuffix(framePath: String): VirtualFile? {
        val nio = findMountedNioBySuffix(framePath) ?: return null
        return LocalFileSystem.getInstance().refreshAndFindFileByNioFile(nio)
    }

    fun findMountedNioBySuffix(framePath: String): Path? {
        val segments = framePath.replace('\\', '/').trim('/').split('/')
        for (root in mountedRoots) {
            for (i in segments.indices) {
                val candidate = root.resolve(
                    segments.subList(i, segments.size).joinToString("/"))
                if (Files.isRegularFile(candidate)) return candidate
            }
        }
        return null
    }

    fun registerForTests(root: Path) { mountedRoots.add(root) }
    fun unregisterForTests(root: Path) { mountedRoots.remove(root) }

    private fun sealReadOnly(dir: Path) {
        Files.walk(dir).use { walk ->
            walk.filter { Files.isRegularFile(it) }
                .forEach { it.toFile().setWritable(false) }
        }
    }

    private fun sha256(file: Path): String {
        val md = MessageDigest.getInstance("SHA-256")
        Files.newInputStream(file).use { ins ->
            val buf = ByteArray(1 shl 16)
            while (true) {
                val n = ins.read(buf)
                if (n < 0) break
                md.update(buf, 0, n)
            }
        }
        return md.digest().joinToString("") { "%02x".format(it) }
    }
}

/**
 * The sourced-vs-sourceless decision (8.1.6 - 8.1.8): archive source is THE
 * path; a sourceless archive yields an OFFER, never silence and never an
 * unrequested network call; an opted-in fetch mounts the result marked
 * UNVERIFIED — the tag may not match the built bitcode (spec §6.0.6), and
 * the IDE must say so rather than present it as authoritative.
 */
object LibrarySourceResolver {

    sealed class Outcome {
        data class Mounted(val root: Path, val unverified: Boolean) : Outcome()
        data class OfferFetch(val repoUrl: String) : Outcome()
        object None : Outcome()
    }

    fun resolve(
        cja: Path,
        repoUrl: String?,
        cacheRoot: Path,
        extractor: (Path, Path) -> Int,
        fetcher: (String, Path) -> Path?,
        optInFetch: Boolean = false,
    ): Outcome {
        CajetaMountedSources.mountArchive(cja, cacheRoot, extractor)?.let {
            return Outcome.Mounted(it, unverified = false)
        }
        if (repoUrl.isNullOrBlank()) return Outcome.None
        if (!optInFetch) return Outcome.OfferFetch(repoUrl)
        val fetched = fetcher(repoUrl, cacheRoot) ?: return Outcome.None
        return Outcome.Mounted(fetched, unverified = true)
    }
}
