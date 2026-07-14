// The diagnostic engine: recoverable semantic errors are *reported* here and
// accumulated, then finalized (sorted by span, deduped, capped) and emitted at
// the end of analysis — instead of thrown-and-aborted (diagnostic-engine-spec).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace cajeta {

    struct CollectedDiagnostic {
        std::string severity;  // "error" | "warning" | "note"
        std::string code;
        std::string message;
        std::string file;
        int line = -1;         // 1-based; <= 0 = no location
        int column = -1;       // 1-based
    };

    class DiagnosticEngine {
    public:
        static constexpr int CAP = 100;

        DiagnosticEngine() = default;
        explicit DiagnosticEngine(bool suppressed) : suppressed_(suppressed) {}

        // Unit-7 sweep fix: the JIT test harness installs an engine ONLY to
        // capture warnings — errors must keep THROWING (fail-loud tests rely
        // on it; a collected error lets codegen run into null types and
        // SIGSEGV). The CLI lint path keeps full collect-and-continue.
        bool collectsErrors() const { return collectErrors_; }
        void setCollectErrors(bool v) { collectErrors_ = v; }

        // Report a recoverable diagnostic. No-op when suppressed (context files).
        void report(const std::string& severity, const std::string& code,
                    const std::string& message, const std::string& file = "",
                    int line = -1, int column = -1);

        bool hasErrors() const { return errorSeen_; }
        std::size_t count() const { return diags_.size(); }

        // Deduped (by file,line,column,code), sorted by span (file,line,column),
        // capped at CAP with a trailing "…and N more" note.
        std::vector<CollectedDiagnostic> finalize() const;

        // finalize() then write each to stderr in the active format.
        void emit(bool json) const;

        // Active-engine cursor, so report sites deep in analysis reach it without
        // threading it through every call.
        static DiagnosticEngine* active();
        static void setActive(DiagnosticEngine* e);

    private:
        bool suppressed_ = false;
        bool collectErrors_ = true;
        bool errorSeen_ = false;
        std::vector<CollectedDiagnostic> diags_;
    };

} // namespace cajeta
