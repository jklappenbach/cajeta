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
            val stderr = runCompiler(compilerPath, tempFile) ?: return emptyList()
            return parseDiagnostics(stderr, bufferText)
        } finally {
            runCatching { Files.deleteIfExists(tempFile) }
        }
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

    private fun runCompiler(compilerPath: String, file: Path): String? {
        return try {
            // --emit=ir keeps it cheap (no linker / native codegen) while still
            // running the diagnostic passes; --diag-format=json makes stderr NDJSON.
            val args = listOf(
                compilerPath,
                "--emit=ir",
                "--diag-format=json",
                file.toString(),
            )
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
