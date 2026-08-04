package dev.cajeta.idea.xref

import com.intellij.openapi.application.PathManager
import com.intellij.openapi.diagnostic.Logger
import dev.cajeta.idea.buildtool.ManifestScan
import dev.cajeta.idea.settings.CajetaSettings
import java.io.File
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.Paths
import java.util.concurrent.TimeUnit
import kotlin.streams.asSequence

/**
 * Production wiring for [CajetaMountedSources] (ide-symbol-index Unit 8):
 * the real subprocess extractors. Kept apart from the mount/cache logic so
 * that logic stays testable without the compiler binary.
 */
object CajetaSourceMountGlue {

    private val log = Logger.getInstance(CajetaSourceMountGlue::class.java)

    fun defaultCacheRoot(): Path =
        Paths.get(PathManager.getSystemPath(), "cajeta-sources")

    /** `cajeta archive extract <cja> -C <dest> <cajeta-glob>` → files extracted. */
    fun archiveExtractor(compilerPath: String): (Path, Path) -> Int = { cja, dest ->
        run(listOf(compilerPath, "archive", "extract", cja.toString(),
                   "-C", dest.toString(), "**/*.cajeta"))
        Files.walk(dest).use { w ->
            w.asSequence().count {
                Files.isRegularFile(it) && it.toString().endsWith(".cajeta")
            }
        }
    }

    /** `cajeta stdlib extract <dest>` (Unit 4). */
    fun stdlibExtractor(compilerPath: String): (Path) -> Int = { dest ->
        run(listOf(compilerPath, "stdlib", "extract", dest.toString()))
    }

    /**
     * The project's resolved dependency `.cja` archives (§8.3.1): the resolver
     * places each at `<projectBase>/.cajeta/cache/artifacts/<sha256>.cja` — the
     * content-hash path it hands the compiler as `--classpath`. Both the source
     * mount ([CajetaSourceMountStartup]) and the whole-root export
     * ([CajetaXrefRebuildAction], which passes them as `--classpath` so the
     * export carries the dependency's declarations) discover deps here. Empty
     * when the project has never been resolved/built.
     */
    fun dependencyArchives(basePath: String?): List<Path> {
        if (basePath == null) return emptyList()
        val base = File(basePath)
        // Every sub-project's artifacts dir, not just the root's. A repo opened
        // at its root (cajeta-logging, with the consumer under samples/tour) is
        // ordinary, and looking only at <base>/.cajeta left those dependencies
        // unmounted and their imports unresolved — Julian, 2026-07-30.
        val dirs = LinkedHashSet<File>()
        dirs.add(File(base, ARTIFACTS))
        for (manifest in ManifestScan.findManifests(base)) {
            manifest.parentFile?.let { dirs.add(File(it, ARTIFACTS)) }
        }
        val byName = LinkedHashMap<String, Path>()   // same hash resolved twice = one archive
        for (dir in dirs) {
            if (!dir.isDirectory) continue
            dir.listFiles { f -> f.isFile && f.name.endsWith(".cja") }
                ?.sortedBy { it.name }
                ?.forEach { byName.putIfAbsent(it.name, it.toPath()) }
        }
        return byName.values.toList()
    }

    private const val ARTIFACTS = ".cajeta/cache/artifacts"

    /** The compiler identity the stdlib cache keys on: `cajeta --version`. */
    fun compilerIdentity(compilerPath: String): String? = try {
        val p = ProcessBuilder(compilerPath, "--version")
            .redirectErrorStream(true).start()
        val out = p.inputStream.bufferedReader().readText()
        p.waitFor(10, TimeUnit.SECONDS)
        out.lineSequence().firstOrNull { it.isNotBlank() }?.trim()
    } catch (e: Exception) {
        log.warn("cajeta --version failed: ${e.message}"); null
    }

    /**
     * Mount the configured compiler's stdlib source (idempotent, cheap on
     * cache hit). Called on debug-session start so stops in stdlib frames
     * land in real files (8.2.4 / spec §6.2, §6.3).
     */
    fun ensureStdlibMounted(): Path? {
        val compilerPath = CajetaSettings.instance.compilerPath
        if (compilerPath.isBlank() || !File(compilerPath).canExecute()) return null
        val identity = compilerIdentity(compilerPath) ?: return null
        return CajetaMountedSources.mountStdlib(
            identity, defaultCacheRoot(), stdlibExtractor(compilerPath))
    }

    private fun run(argv: List<String>): Int = try {
        val p = ProcessBuilder(argv).redirectErrorStream(true).start()
        p.inputStream.bufferedReader().readText()
        if (p.waitFor(120, TimeUnit.SECONDS)) p.exitValue() else -1
    } catch (e: Exception) {
        log.warn("${argv.firstOrNull()} failed: ${e.message}"); -1
    }
}
