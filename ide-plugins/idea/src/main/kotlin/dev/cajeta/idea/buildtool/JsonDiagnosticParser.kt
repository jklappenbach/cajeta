package dev.cajeta.idea.buildtool

import dev.cajeta.idea.debugger.Json
import dev.cajeta.idea.lint.Diagnostic

/**
 * One compiler diagnostic parsed from the `--diag-format=json` NDJSON stream
 * (json-diagnostics-spec §3). `code`/`file`/`line`/`column` are null when the
 * compiler emitted JSON null (or omitted them); `line`/`column` are 1-based as
 * emitted (`Diagnostics.h::emitJsonDiagnostic`). Severity uses the single
 * degraded-lint vocabulary (`lint/Diagnostic.Severity`).
 */
data class JsonDiagnostic(
    val severity: Diagnostic.Severity,
    val code: String?,
    val message: String,
    val file: String?,
    val line: Int?,
    val column: Int?,
)

/**
 * Parses one NDJSON diagnostic line, reusing the bundled dependency-free [Json]
 * DOM (which decodes RFC-8259 escapes). Returns null for a line that is not a
 * JSON diagnostic object — a non-diagnostic console line, malformed JSON, or a
 * JSON object without a recognized `severity` — so the caller leaves it as plain
 * console output (no regex fallback; json-diagnostics-spec §1.4.4). Pure.
 */
object JsonDiagnosticParser {

    fun parse(line: String): JsonDiagnostic? {
        val trimmed = line.trim()
        if (trimmed.isEmpty() || trimmed[0] != '{') return null
        val root = try {
            Json.parse(trimmed)
        } catch (_: Exception) {
            return null
        }
        if (root !is Json.Obj) return null
        val severity = severityOf((root.opt("severity") as? Json.Str)?.value) ?: return null
        return JsonDiagnostic(
            severity = severity,
            code = (root.opt("code") as? Json.Str)?.value,
            message = (root.opt("message") as? Json.Str)?.value ?: "",
            file = (root.opt("file") as? Json.Str)?.value,
            line = (root.opt("line") as? Json.Num)?.value?.toInt(),
            column = (root.opt("column") as? Json.Num)?.value?.toInt(),
        )
    }

    private fun severityOf(s: String?): Diagnostic.Severity? = when (s) {
        "error" -> Diagnostic.Severity.ERROR
        "warning" -> Diagnostic.Severity.WARNING
        "note" -> Diagnostic.Severity.WEAK_WARNING
        else -> null
    }
}
