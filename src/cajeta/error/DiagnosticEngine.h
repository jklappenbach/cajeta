// The diagnostic engine: recoverable semantic errors are reported here and accumulated,
// then finalized (sorted by span, deduped, capped) and emitted at the end of analysis.
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

        // False when errors must keep THROWING: a collected error lets codegen run into null types.
        bool collectsErrors() const { return collectErrors_; }
        void setCollectErrors(bool v) { collectErrors_ = v; }

        void report(const std::string& severity, const std::string& code,
                    const std::string& message, const std::string& file = "",
                    int line = -1, int column = -1);

        bool hasErrors() const { return errorSeen_; }
        std::size_t count() const { return diags_.size(); }

        // Deduped by (file,line,column,code), sorted by span, and capped with a trailing note.
        std::vector<CollectedDiagnostic> finalize() const;

        void emit(bool json) const;

        // Active-engine cursor, so report sites deep in analysis reach it without threading it through.
        static DiagnosticEngine* active();
        static void setActive(DiagnosticEngine* e);

    private:
        bool suppressed_ = false;
        bool collectErrors_ = true;
        bool errorSeen_ = false;
        std::vector<CollectedDiagnostic> diags_;
    };

} // namespace cajeta
