// Compiler diagnostics: located exceptions, the --diag-hints "did you mean"
// helpers (pure; the flag is checked at the call site), and the NDJSON record
// stream behind --diag-format=json (docs/CompilerModes.md).

#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "Exception.h"

namespace antlr4 { class Token; }

namespace cajeta {

    // An Exception carrying `token`'s 1-based location and the active module's
    // source path; a null token yields an unlocated Exception.
    Exception locatedException(antlr4::Token* token,
                               const std::string& message,
                               const std::string& errorId);

    // The same for AST-node throw sites, whose line/column are already 1-based.
    Exception locatedException(int line, int column,
                               const std::string& message,
                               const std::string& errorId);

    // Report a recoverable semantic error to the active DiagnosticEngine and
    // RETURN, so the caller can recover; with no engine active it throws.
    void reportOrThrow(antlr4::Token* token,
                       const std::string& errorId, const std::string& message);
    void reportOrThrow(int line, int column,
                       const std::string& errorId, const std::string& message);


    // Levenshtein edit distance, insert/delete/substitute each weighted 1.
    int levenshteinDistance(const std::string& a, const std::string& b);

    // Candidates within `maxDistance` of `target`, at most `maxSuggestions` of
    // them, by ascending distance and then alphabetically. Empty when none fit.
    std::vector<std::string> pickSimilar(
        const std::string& target,
        const std::vector<std::string>& candidates,
        int maxDistance = 2,
        std::size_t maxSuggestions = 3);

    // A "did you mean `foo`?" / "did you mean one of: ...?" suffix to append to
    // a message; empty input gives "", so callers can concatenate blindly.
    std::string formatDidYouMean(const std::vector<std::string>& suggestions);

    // Who produced the records being written RIGHT NOW — per-RECORD provenance,
    // unlike the per-STREAM `jsonProducer()`. Defaults to the compiler.
    void setJsonSource(const std::string& name, const std::string& version);
    const std::string& jsonSourceName();
    const std::string& jsonSourceVersion();

    // Stamp records as `name`@`version` for this scope, then restore. RAII, not
    // set/reset: the ingest path returns early on every malformed record.
    class JsonSourceScope {
    public:
        JsonSourceScope(std::string name, std::string version);
        ~JsonSourceScope();
        JsonSourceScope(const JsonSourceScope&) = delete;
        JsonSourceScope& operator=(const JsonSourceScope&) = delete;
    private:
        std::string prevName;
        std::string prevVersion;
    };

    // A named value a plugin published (`${id.key}` in the manifest).
    void emitJsonOutput(const std::string& key, const std::string& value);

    // Plugin text for a human, verbatim: no prefix and no added newline.
    void emitJsonWrite(const std::string& text);

    // One diagnostic as a self-contained NDJSON line on stderr; severity is
    // "error" | "warning" | "note", and empty or non-positive fields are null.
    void emitJsonDiagnostic(const std::string& severity,
                            const std::string& code,
                            const std::string& message,
                            const std::string& file = "",
                            int line = -1,
                            int column = -1);

    // Process-wide gate for the progress records below, set once at flag parse.
    void setJsonProgressEnabled(bool enabled);
    bool jsonProgressEnabled();

    // The `stream` record's `producer`, defaulted from build provenance rather
    // than argv so an in-process stream names what the binary would.
    void setJsonProducer(const std::string& producer);
    const std::string& jsonProducer();

    // Resolve `--diag-format=json` from raw argv BEFORE any verb dispatches —
    // `jit-run` and `dap` return before the main flag loop. True when selected.
    bool resolveDiagFormatFromArgv(int argc, const char* argv[]);

    // Announce the stream with one `{"kind":"stream",...}` record, before any
    // other record and at most once per process; emitted even for a silent run,
    // so a consumer can tell "clean" from "died before producing anything".
    void emitStreamRecordOnce();

    // The unlatched form, for the lint driver: each response is its own stream,
    // and a warm server must replay what a one-shot process would emit.
    void emitStreamRecord();

    // MAJOR bumps on a breaking change and consumers must REFUSE an unknown
    // one; MINOR bumps on an added record kind or field, which they may ignore.
    constexpr int kJsonlSchemaMajor = 1;
    constexpr int kJsonlSchemaMinor = 1;

    // One compile-phase record on the diagnostics stream, flushed as the phase
    // begins; `phase` is a stable id and `state` is "start" or "finish".
    void emitJsonProgress(const std::string& phase,
                          const std::string& state,
                          const std::string& label,
                          long long elapsedMs = -1);

    // Narration that is not a diagnostic; `level` is "info" | "warn" | "debug"
    // and the message is carried VERBATIM, because people read these.
    void emitJsonLog(const std::string& level, const std::string& message);
    // The same narration, routed: a `log` record under JSON, otherwise `text`
    // to stderr unchanged. `text` must carry its own trailing newline.
    void logLine(const std::string& level, const std::string& text);

    // The terminal record — exactly one, last, so the stream alone answers "did
    // it work". `status` is "ok" | "error"; an empty `message` is omitted.
    void emitJsonResult(const std::string& status, const std::string& message = "");

    // One cache-hit record naming the re-published artifact: a cached build runs
    // no compiler, so this is the only thing it has to say.
    void emitJsonCacheHit(const std::string& artifact);

    // RAII phase marker: `start` on construction, `finish` with the measured
    // duration on destruction, so a throw still closes its phase.
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
