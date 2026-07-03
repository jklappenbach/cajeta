package dev.cajeta.idea.lint

import com.intellij.openapi.diagnostic.Logger
import com.intellij.openapi.util.TextRange
import dev.cajeta.idea.buildtool.JsonDiagnosticParser
import dev.cajeta.idea.settings.CajetaSettings
import java.io.File
import java.io.IOException
import java.nio.charset.StandardCharsets
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardOpenOption
import java.util.concurrent.TimeUnit

/**
 * Spawns cajetac against a source buffer with `--diag-format=json` and turns the
 * NDJSON stderr into editor [Diagnostic]s at precise ranges (json-diagnostics
 * spec §4). JSON-only — the compiler always emits structured diagnostics under
 * the flag, so there is no regex scraping and no text fallback.
 */
object CajetacRunner {

    private val log = Logger.getInstance(CajetacRunner::class.java)

    fun lint(filePath: String, bufferText: String): List<Diagnostic> {
        val compilerPath = CajetaSettings.instance.compilerPath
        if (compilerPath.isBlank() || !File(compilerPath).canExecute()) {
            log.debug("cajetac not configured or not executable at: $compilerPath")
            return emptyList()
        }

        val tempFile = stageBufferToTemp(filePath, bufferText) ?: return emptyList()
        try {
            // Resolve cross-file references against the project: pass the source
            // root (derived from the file's package) and shadow the on-disk twin
            // so the staged buffer replaces it (lint-source-root-spec §5/§7).
            val sourceRoot = sourceRootOf(filePath, bufferText)
            val stderr = runCompiler(compilerPath, tempFile, sourceRoot, filePath)
                ?: return emptyList()
            return parseDiagnostics(stderr, bufferText)
        } finally {
            runCatching { Files.deleteIfExists(tempFile) }
        }
    }

    private val PACKAGE_RE = Regex("""(?m)^\s*package\s+([A-Za-z_][\w.]*)\s*;""")

    /** The project source root for [filePath]: its directory with the file's
     *  `package a.b.c;` path segments stripped off the tail (so `<root>/a/b/c/
     *  Foo.cajeta` → `<root>`). Falls back to the file's own directory when there
     *  is no package or the on-disk layout doesn't match the package. */
    internal fun sourceRootOf(filePath: String, bufferText: String): String {
        val parent = File(filePath).parentFile ?: return File(filePath).absolutePath
        val pkg = PACKAGE_RE.find(bufferText)?.groupValues?.get(1)
        if (pkg.isNullOrBlank()) return parent.path
        var root: File = parent
        for (seg in pkg.split('.').asReversed()) {
            val up = root.parentFile
            if (root.name == seg && up != null) root = up else return parent.path
        }
        return root.path
    }

    private fun stageBufferToTemp(filePath: String, text: String): Path? {
        return try {
            val base = File(filePath).nameWithoutExtension
            val temp = Files.createTempFile("cajeta-lint-$base-", ".cajeta")
            Files.write(
                temp,
                text.toByteArray(StandardCharsets.UTF_8),
                StandardOpenOption.WRITE,
                StandardOpenOption.TRUNCATE_EXISTING,
            )
            temp
        } catch (e: IOException) {
            log.warn("Failed to stage buffer for linting: ${e.message}")
            null
        }
    }

    /** The single-file lint invocation: `cajeta --lint <file> --diag-format=json`
     *  (compiler-lint-mode spec §4). Runs the diagnostic passes on one file with
     *  no codegen/emit/entry-method — unlike a full compile, which needs three
     *  positionals and would just print usage. */
    internal fun lintArgv(
        compilerPath: String,
        file: String,
        sourceRoot: String? = null,
        shadow: String? = null,
    ): List<String> {
        val args = mutableListOf(compilerPath, "--lint", file, "--diag-format=json")
        if (sourceRoot != null) {
            args += listOf("--source-root", sourceRoot)
            if (shadow != null) args += listOf("--shadow", shadow)
        }
        return args
    }

    private fun runCompiler(
        compilerPath: String,
        file: Path,
        sourceRoot: String? = null,
        shadow: String? = null,
    ): String? {
        return try {
            val args = lintArgv(compilerPath, file.toString(), sourceRoot, shadow)
            val pb = ProcessBuilder(args).redirectErrorStream(false)
            val process = pb.start()
            process.outputStream.close()
            val finished = process.waitFor(10, TimeUnit.SECONDS)
            if (!finished) {
                process.destroyForcibly()
                log.warn("cajetac timed out after 10s on ${file.fileName}")
                return null
            }
            process.errorStream.bufferedReader(StandardCharsets.UTF_8).use { it.readText() }
        } catch (e: IOException) {
            log.warn("Failed to spawn cajetac: ${e.message}")
            null
        } catch (e: InterruptedException) {
            Thread.currentThread().interrupt()
            null
        }
    }

    internal fun parseDiagnostics(stderr: String, bufferText: String): List<Diagnostic> {
        val diagnostics = mutableListOf<Diagnostic>()
        for (line in stderr.lineSequence()) {
            val d = JsonDiagnosticParser.parse(line) ?: continue
            diagnostics += Diagnostic(
                ruleId = d.code ?: "cajeta",
                severity = d.severity,
                message = d.message,
                range = rangeFor(d.line, d.column, bufferText),
            )
        }
        return diagnostics
    }

    /** 1-based line/column from the compiler → a buffer [TextRange]: the token at
     *  the position (letters/digits/underscore), else to end of line; a
     *  positionless diagnostic falls back to the first non-blank line. */
    private fun rangeFor(line1: Int?, col1: Int?, buffer: String): TextRange {
        if (line1 == null) return firstNonBlankRange(buffer)
        val start = offsetOf(buffer, line1, col1 ?: 1)
        if (start < 0 || start >= buffer.length) return firstNonBlankRange(buffer)
        var end = start
        while (end < buffer.length && (buffer[end].isLetterOrDigit() || buffer[end] == '_')) end++
        if (end == start) {
            val nl = buffer.indexOf('\n', start)
            end = if (nl < 0) buffer.length else nl
            if (end == start) end = minOf(start + 1, buffer.length)
        }
        return TextRange(start, end)
    }

    private fun offsetOf(buffer: String, line1: Int, col1: Int): Int {
        var offset = 0
        var ln = 1
        while (ln < line1) {
            val nl = buffer.indexOf('\n', offset)
            if (nl < 0) return -1
            offset = nl + 1
            ln++
        }
        return (offset + (col1 - 1)).coerceIn(0, buffer.length)
    }

    private fun firstNonBlankRange(buffer: String): TextRange {
        val firstNonBlank = buffer.lineSequence().withIndex().firstOrNull { it.value.isNotBlank() }
            ?: return TextRange(0, minOf(1, buffer.length))
        val lineStart = buffer.split('\n').take(firstNonBlank.index).sumOf { it.length + 1 }
        return TextRange(lineStart, lineStart + firstNonBlank.value.length)
    }
}
