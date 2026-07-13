package dev.cajeta.idea.lint

/**
 * Everything one `cajeta --lint` subprocess returned, demultiplexed
 * (ide-symbol-index §1.5.2): the diagnostics the annotator paints, and the
 * linted buffer's xref records for the symbol index. One invocation per edit —
 * the xref side must never cost a second compiler run.
 */
data class LintOutput(
    val diagnostics: List<Diagnostic>,
    val xref: XrefStream,
)
