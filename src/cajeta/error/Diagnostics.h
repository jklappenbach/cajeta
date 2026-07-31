// Compiler-diagnostic helpers for the --diag-hints feature
// (docs/CompilerModes.md § --diag-hints). When the flag is on,
// error and warning messages that name an unresolved identifier are
// enriched with "did you mean..." suggestions drawn from contextually
// available names. When the flag is off, callers skip the lookup
// entirely and emit lean error text.
//
// Surface is intentionally small: each helper is pure, takes its
// inputs by reference, and returns the result by value. Gating on
// the diagHints flag happens at the call site (so off-mode callers
// don't even invoke the distance computation).
//
// Distance metric is Levenshtein (single-character edit distance);
// matches at distance ≤ 2 are typically typo-shaped without
// over-reaching into unrelated names.

#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "Exception.h"

namespace antlr4 { class Token; }

namespace cajeta {

    // Build a cajeta::Exception carrying `token`'s source location (1-based
    // line/column) plus the active module's source path — for
    // located-semantic-diagnostics. A null token yields an unlocated Exception.
    Exception locatedException(antlr4::Token* token,
                               const std::string& message,
                               const std::string& errorId);

    // Overload for AST-node throw sites: explicit 1-based line/column (from
    // AbstractSyntaxNode::getSourceLine()/getSourceColumn() + 1) + the active
    // module's source path.
    Exception locatedException(int line, int column,
                               const std::string& message,
                               const std::string& errorId);

    // Migration primitive (diagnostic-engine): report a recoverable semantic
    // error to the active DiagnosticEngine and return (so the caller recovers,
    // e.g. with CajetaType::error()). When no engine is active — full compile
    // that hasn't opted into collect-and-continue — it throws locatedException
    // instead, preserving today's abort-on-first behavior. Two location forms.
    void reportOrThrow(antlr4::Token* token,
                       const std::string& errorId, const std::string& message);
    void reportOrThrow(int line, int column,
                       const std::string& errorId, const std::string& message);


    // Compute the Levenshtein edit distance between two strings.
    // O(|a| · |b|) time and O(min(|a|, |b|)) space. Distance includes
    // the conventional insert / delete / substitute operations, each
    // weighted 1.
    int levenshteinDistance(const std::string& a, const std::string& b);

    // Pick names from `candidates` that are close to `target`. Returns
    // a sorted list of suggestions by ascending distance, ties broken
    // alphabetically. Filters: distance ≤ maxDistance, returns at most
    // maxSuggestions entries. Empty result if no candidate is close
    // enough — caller should skip the "did you mean..." preamble in
    // that case.
    std::vector<std::string> pickSimilar(
        const std::string& target,
        const std::vector<std::string>& candidates,
        int maxDistance = 2,
        std::size_t maxSuggestions = 3);

    // Format a list of suggestions into a "did you mean..." suffix
    // appropriate to append to an error message. Empty input → empty
    // string (so the caller can string-concat unconditionally and
    // get a clean message when there are no good matches). Single
    // suggestion: "did you mean `foo`?". Multiple: "did you mean one
    // of: `foo`, `bar`, `baz`?". Backticks frame each name so they
    // survive line-wrapping cleanly in terminal output.
    std::string formatDidYouMean(const std::vector<std::string>& suggestions);

    // Emit one machine-readable diagnostic as a single self-contained NDJSON
    // line to stderr — the payload of `--diag-format=json` (docs/CompilerModes.md
    // § --diag-format). Fields: severity ("error" | "warning" | "note"), code
    // (compiler error id; "" → JSON null), message, file (source path; "" → JSON
    // null), line / column (1-based; ≤ 0 → JSON null). One line per call so a
    // consumer (the IDE plugin, build tool) can parse diagnostics incrementally
    // as they stream.
    void emitJsonDiagnostic(const std::string& severity,
                            const std::string& code,
                            const std::string& message,
                            const std::string& file = "",
                            int line = -1,
                            int column = -1);

    // Process-wide gate for the phase-progress records below. Set once from the
    // flag parse (`--diag-format=json`). Mirrors the build tool's
    // buildtool::setDiagnosticFormat toggle: the phase markers live deep inside
    // Compiler::compile, and threading a bool through every phase would buy
    // nothing over a single process-scoped switch.
    void setJsonProgressEnabled(bool enabled);
    bool jsonProgressEnabled();

    // Who is emitting, for the `stream` record's `producer`. Defaults to
    // "cajeta <CAJETA_VERSION>" from build provenance — NOT from argv — so a
    // stream written in-process (the lint reuse harness) names the same
    // producer as one written by the `cajeta` binary. Override only if some
    // other tool ever emits this format.
    void setJsonProducer(const std::string& producer);
    const std::string& jsonProducer();

    // Resolve `--diag-format=json` from a raw argv, BEFORE any verb dispatches
    // (compiler-jsonl 5.1.2). `jit-run` and `dap` return long before the main
    // flag-parse loop, which is exactly why the flag used to mean something
    // different depending on the verb. Returns true when JSON was selected.
    bool resolveDiagFormatFromArgv(int argc, const char* argv[]);

    // Announce the stream: one `{"kind":"stream","major":M,"minor":N,
    // "producer":"…"}` record, emitted BEFORE any other record and at most once
    // per process (compiler-jsonl 2.1.3). It is emitted even when the run has
    // nothing else to say, so a consumer can tell "clean" from "the process
    // died before producing anything" (spec 2.2.4). No-op in text mode, and
    // no-op on a second call. The producer comes from setJsonProducer, so this
    // stays free of the build-stamped version macros.
    void emitStreamRecordOnce();

    // The unlatched form: announces a stream every time it is called. Used by
    // the lint driver, where each response is its own stream and the warm
    // server must replay a payload byte-identical to a fresh one-shot process
    // (lint-server-spec 1.4.1) — a process-lifetime latch would silently drop
    // the record from the second request onward.
    void emitStreamRecord();

    // The stream's schema version (compiler-jsonl 2.1.3/2.1.4). MAJOR bumps on
    // any breaking change and consumers are required to REFUSE an unknown one
    // rather than guess; MINOR bumps when a record kind or field is ADDED,
    // which existing consumers skip or ignore (2.1.5/2.1.6).
    constexpr int kJsonlSchemaMajor = 1;
    constexpr int kJsonlSchemaMinor = 0;

    // Emit one compile-phase progress record as an NDJSON line to stderr, on the
    // SAME stream as the diagnostics above and only under `--diag-format=json`.
    // Fields: kind ("progress" — the discriminator, which EVERY record now
    // carries, diagnostics included, so consumers dispatch instead of sniffing
    // for a `severity` field), phase (stable id:
    // "prescan" | "parse" | "resolve" | "codegen" | "emit" | "link"), state
    // ("start" | "finish"), label (human-readable phase name), and, on finish,
    // elapsedMs. One line per call, flushed, so the IDE sees each phase as it
    // begins rather than after the build ends.
    void emitJsonProgress(const std::string& phase,
                          const std::string& state,
                          const std::string& label,
                          long long elapsedMs = -1);

    // Narration that is not a diagnostic: progress notes, failure explanations,
    // debugger chatter (compiler-jsonl 3.1.5). `level` is "info" | "warn" |
    // "debug". The message is carried VERBATIM (spec 9.2) — these lines are
    // read by people, and typed records for e.g. `[step-armed]` would be schema
    // surface with no consumer.
    void emitJsonLog(const std::string& level, const std::string& message);
    // The same narration, routed: under `--diag-format=json` it becomes a `log`
    // record; otherwise `text` goes to stderr exactly as it always has. This is
    // what keeps text mode byte-identical (spec 1.4.1) while giving the JSON
    // console something it can filter and tint. `text` must include its own
    // trailing newline, matching the `std::cerr` it replaces.
    void logLine(const std::string& level, const std::string& text);

    // Terminal record: exactly one, last, so "did it work" is answerable from
    // the stream alone rather than inferred from an exit code plus silence
    // (compiler-jsonl 3.1.6 / 9.4). `status` is "ok" | "error"; `message`
    // explains a failure and is omitted when empty.
    void emitJsonResult(const std::string& status, const std::string& message = "");

    // Emit one cache-hit record as an NDJSON line to stderr, on the same stream
    // as the diagnostics and phase records. A cached build runs no compiler at
    // all, so it emits no phases — without this the IDE shows an instant green
    // check and an empty tree, which reads as "the build did nothing". Fields:
    // kind ("cache"), state ("hit"), artifact (the re-published output path).
    void emitJsonCacheHit(const std::string& artifact);

    // RAII phase marker: emits `start` on construction and `finish` (with the
    // measured duration) on destruction, so an early return or a thrown
    // diagnostic still closes the phase it was raised in. A no-op unless the
    // compiler is running under `--diag-format=json`.
    class ProgressPhase {
    public:
        ProgressPhase(std::string phase, std::string label);
        ~ProgressPhase();
        ProgressPhase(const ProgressPhase&) = delete;
        ProgressPhase& operator=(const ProgressPhase&) = delete;

    private:
        std::string phase;
        std::string label;
        bool active;
        std::chrono::steady_clock::time_point startedAt;
    };

} // namespace cajeta
