package dev.cajeta.idea.lint

import com.intellij.openapi.diagnostic.Logger
import com.intellij.openapi.util.TextRange
import dev.cajeta.idea.settings.CajetaSettings
import java.io.File
import java.io.IOException
import java.nio.charset.StandardCharsets
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardOpenOption
import java.util.concurrent.TimeUnit

/**
 * Spawns cajetac against a source buffer and parses its diagnostic
 * output. v0.1 ships in degraded mode: cajetac doesn't yet have
 * `--lint --diag-format=json --stdin`, so we write the buffer to a
 * temp file, invoke the compiler against it, and regex-parse the
 * human-readable warnings off stderr.
 *
 * TODO(cajetac-json-diagnostics): replace this with the structured
 * JSON path once the compiler-side refactor lands (see
 * ide-plugins/idea/Plan.md § Follow-up).
 */
object CajetacRunner {

    private val log = Logger.getInstance(CajetacRunner::class.java)

    /**
     * Regex matches lines like:
     *     warning: [uncaught-throws] call to fetch can throw ...
     * Both `warning:` and `warning —` forms are tolerated since the
     * compiler currently emits both.
     */
    internal val WARNING_RE = Regex(
        """^warning(?:\s*[:—-])\s*\[(?<id>[^\]]+)\]\s*(?<msg>.+)$""",
    )

    fun lint(filePath: String, bufferText: String): List<Diagnostic> {
        val compilerPath = CajetaSettings.instance.compilerPath
        if (compilerPath.isBlank() || !File(compilerPath).canExecute()) {
            log.debug("cajetac not configured or not executable at: $compilerPath")
            return emptyList()
        }

        val tempFile = stageBufferToTemp(filePath, bufferText) ?: return emptyList()
        try {
            val stderr = runCompiler(compilerPath, tempFile) ?: return emptyList()
            return parseStderr(stderr, bufferText)
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
            // Run the compiler with --emit=ir to keep it cheap (no linker,
            // no native codegen output) while still triggering the lint
            // pass. We don't care about the output artifact.
            val args = listOf(
                compilerPath,
                "--emit=ir",
                "--diag-verbosity=normal",
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

    private fun parseStderr(stderr: String, bufferText: String): List<Diagnostic> {
        // Degraded mode: regex doesn't carry offsets, so we attach each
        // diagnostic to the first line that mentions any quoted token
        // from the message, falling back to line 1.
        val diagnostics = mutableListOf<Diagnostic>()
        for (line in stderr.lineSequence()) {
            val match = WARNING_RE.matchEntire(line.trim()) ?: continue
            val id = match.groups["id"]?.value ?: continue
            val msg = match.groups["msg"]?.value ?: continue
            val range = guessRangeFromMessage(msg, bufferText)
            diagnostics += Diagnostic(
                ruleId = id,
                severity = Diagnostic.Severity.WARNING,
                message = msg,
                range = range,
            )
        }
        return diagnostics
    }

    /**
     * Try to attach the warning to a meaningful place in the source.
     * Looks for the first identifier-shaped token in the message and
     * finds the first occurrence of it in the buffer. Falls back to
     * the first line.
     */
    private fun guessRangeFromMessage(message: String, bufferText: String): TextRange {
        val identifierRe = Regex("""\b[a-zA-Z_][a-zA-Z0-9_]*\b""")
        for (m in identifierRe.findAll(message)) {
            val token = m.value
            if (token.length < 3 || token in COMMON_WORDS) continue
            val pos = bufferText.indexOf(token)
            if (pos >= 0) return TextRange(pos, pos + token.length)
        }
        // Fallback: highlight first non-blank line.
        val firstNonBlank = bufferText.lineSequence()
            .withIndex()
            .firstOrNull { it.value.isNotBlank() }
        return if (firstNonBlank != null) {
            val lineStart = bufferText.split('\n').take(firstNonBlank.index).sumOf { it.length + 1 }
            val lineEnd = lineStart + firstNonBlank.value.length
            TextRange(lineStart, lineEnd)
        } else {
            TextRange(0, minOf(1, bufferText.length))
        }
    }

    private val COMMON_WORDS = setOf(
        "the", "and", "can", "throw", "throws", "but", "neither", "nor",
        "call", "to", "from", "warning", "error", "method", "class",
        "into", "with", "for", "this", "that", "not", "via",
    )
}
