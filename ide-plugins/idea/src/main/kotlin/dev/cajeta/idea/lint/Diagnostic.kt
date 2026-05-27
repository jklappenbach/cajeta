package dev.cajeta.idea.lint

import com.intellij.openapi.util.TextRange

/**
 * Diagnostic surfaced to the IDE. In degraded mode (stderr regex) the
 * range is whole-line; once cajetac emits structured JSON we'll fill
 * in precise spans. See LintRules.md for the full rule catalog.
 */
data class Diagnostic(
    val ruleId: String,
    val severity: Severity,
    val message: String,
    val range: TextRange,
) {
    enum class Severity { ERROR, WARNING, WEAK_WARNING }
}
