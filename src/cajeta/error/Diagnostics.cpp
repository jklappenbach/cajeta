#include "Diagnostics.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <vector>

#include <antlr4-runtime.h>

#include "cajeta/compile/CajetaModule.h"
#include "cajeta/error/DiagnosticEngine.h"

namespace cajeta {

    // The file a located diagnostic belongs to. The active module wins because it
    // is set only in synthesis re-entry, where it names the TRIGGER's file; without
    // the codegen fallback every codegen-time located error reports no file at all.
    static std::string diagnosticFile() {
        if (auto m = CajetaModule::getActiveModule()) return m->getSourcePath();
        if (auto m = CajetaModule::getCurrentCodegenModule()) return m->getSourcePath();
        return {};
    }

    void reportOrThrow(int line, int column,
                       const std::string& errorId, const std::string& message) {
        DiagnosticEngine* eng = DiagnosticEngine::active();
        if (eng && eng->collectsErrors()) {
            eng->report("error", errorId, message, diagnosticFile(), line, column);
        } else {
            throw locatedException(line, column, message, errorId);
        }
    }

    void reportOrThrow(antlr4::Token* token,
                       const std::string& errorId, const std::string& message) {
        int line = token ? static_cast<int>(token->getLine()) : -1;
        int column = token ? static_cast<int>(token->getCharPositionInLine()) + 1 : -1;
        reportOrThrow(line, column, errorId, message);
    }

    Exception locatedException(antlr4::Token* token,
                               const std::string& message,
                               const std::string& errorId) {
        // ANTLR lines are 1-based; columns are 0-based — normalize to 1-based.
        int line = token ? static_cast<int>(token->getLine()) : -1;
        int column = token ? static_cast<int>(token->getCharPositionInLine()) + 1 : -1;
        return Exception(message, errorId, diagnosticFile(), line, column);
    }

    Exception locatedException(int line, int column,
                               const std::string& message,
                               const std::string& errorId) {
        return Exception(message, errorId, diagnosticFile(), line, column);
    }

    namespace {
        // Minimal RFC 8259 string escaping for the NDJSON diagnostic payload.
        std::string jsonEscape(const std::string& s) {
            std::string o;
            o.reserve(s.size() + 8);
            for (unsigned char c : s) {
                switch (c) {
                    case '"':  o += "\\\""; break;
                    case '\\': o += "\\\\"; break;
                    case '\n': o += "\\n";  break;
                    case '\r': o += "\\r";  break;
                    case '\t': o += "\\t";  break;
                    case '\b': o += "\\b";  break;
                    case '\f': o += "\\f";  break;
                    default:
                        if (c < 0x20) {          // other control chars → \u00XX
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                            o += buf;
                        } else {
                            o += static_cast<char>(c);
                        }
                }
            }
            return o;
        }

        void strOrNull(std::string& out, const char* key, const std::string& v) {
            out += "\"";
            out += key;
            out += "\":";
            if (v.empty()) out += "null";
            else { out += "\""; out += jsonEscape(v); out += "\""; }
        }

        // Per-record provenance; the build tool swings it per ingested plugin.
        std::string g_sourceName;      // lazily defaulted, see jsonSourceName
        std::string g_sourceVersion;

        std::string openRecord(const char* kind) {
            std::string o = "{\"kind\":\"";
            o += kind;
            o += "\",";
            // Stamped at the one place a record opens, so no kind can forget it.
            strOrNull(o, "source", jsonSourceName());        o += ",";
            strOrNull(o, "sourceVersion", jsonSourceVersion()); o += ",";
            return o;
        }

        // One line, one flush: a consumer reads each record as it happens.
        void writeRecord(std::string& o) {
            o += "}\n";
            std::cerr << o << std::flush;
        }
    } // namespace

    void emitJsonDiagnostic(const std::string& severity,
                            const std::string& code,
                            const std::string& message,
                            const std::string& file,
                            int line,
                            int column) {
        // Field order and meaning are frozen: a new compiler must not break a plugin.
        std::string o = openRecord("diagnostic");
        strOrNull(o, "severity", severity); o += ",";
        strOrNull(o, "code", code);         o += ",";
        strOrNull(o, "message", message);   o += ",";
        strOrNull(o, "file", file);         o += ",";
        o += "\"line\":";   o += (line   > 0 ? std::to_string(line)   : "null"); o += ",";
        o += "\"column\":"; o += (column > 0 ? std::to_string(column) : "null");
        writeRecord(o);
    }

    namespace {
        bool g_jsonProgress = false;
    }

    void setJsonProgressEnabled(bool enabled) { g_jsonProgress = enabled; }
    bool jsonProgressEnabled() { return g_jsonProgress; }

    namespace {
        // Build provenance, never argv-derived: in-process and subprocess must match.
#ifndef CAJETA_VERSION
#define CAJETA_VERSION "0.0.0-unknown"
#endif
        std::string g_jsonProducer = std::string("cajeta ") + CAJETA_VERSION;
    }

    void setJsonProducer(const std::string& producer) {
        if (!producer.empty()) g_jsonProducer = producer;
    }
    const std::string& jsonProducer() { return g_jsonProducer; }

    // Per-RECORD provenance, as distinct from the per-STREAM producer above.
    const std::string& jsonSourceName() {
        if (g_sourceName.empty()) g_sourceName = "cajeta";
        return g_sourceName;
    }
    const std::string& jsonSourceVersion() {
        if (g_sourceVersion.empty()) g_sourceVersion = CAJETA_VERSION;
        return g_sourceVersion;
    }
    void setJsonSource(const std::string& name, const std::string& version) {
        g_sourceName = name.empty() ? std::string("cajeta") : name;
        g_sourceVersion = version.empty() ? std::string(CAJETA_VERSION) : version;
    }

    JsonSourceScope::JsonSourceScope(std::string name, std::string version)
        : prevName(jsonSourceName()), prevVersion(jsonSourceVersion()) {
        setJsonSource(name, version);
    }
    JsonSourceScope::~JsonSourceScope() {
        setJsonSource(prevName, prevVersion);
    }

    bool resolveDiagFormatFromArgv(int argc, const char* argv[]) {
        // Only the `--diag-format=<value>` form exists, so an exact token compare is
        // the whole grammar; anything else leaves the gate at its text default.
        bool json = false;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--diag-format=json") json = true;
            else if (a == "--diag-format=text") json = false;
        }
        if (json) setJsonProgressEnabled(true);
        return json;
    }

    void emitStreamRecord() {
        // Text mode emits nothing structured, ever (spec 1.4.1).
        if (!jsonProgressEnabled()) return;
        std::string o = openRecord("stream");
        o += "\"major\":"; o += std::to_string(kJsonlSchemaMajor); o += ",";
        o += "\"minor\":"; o += std::to_string(kJsonlSchemaMinor); o += ",";
        strOrNull(o, "producer", jsonProducer());
        writeRecord(o);
    }

    void emitStreamRecordOnce() {
        // For a verb whose whole run is ONE stream. The lint driver takes the
        // unlatched form: a latch would drop its record from request two on.
        static bool emitted = false;
        if (emitted) return;
        emitted = true;
        emitStreamRecord();
    }

    void emitJsonProgress(const std::string& phase,
                          const std::string& state,
                          const std::string& label,
                          long long elapsedMs) {
        std::string o = openRecord("progress");
        strOrNull(o, "phase", phase); o += ",";
        strOrNull(o, "state", state); o += ",";
        strOrNull(o, "label", label);
        if (elapsedMs >= 0) {
            o += ",\"elapsedMs\":";
            o += std::to_string(elapsedMs);
        }
        writeRecord(o);
    }

    void emitJsonLog(const std::string& level, const std::string& message) {
        std::string o = openRecord("log");
        strOrNull(o, "level", level); o += ",";
        strOrNull(o, "message", message);
        writeRecord(o);
    }

    void logLine(const std::string& level, const std::string& text) {
        if (jsonProgressEnabled()) {
            // The trailing newline belongs to the text form, not to the message.
            std::string msg = text;
            while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
                msg.pop_back();
            emitJsonLog(level, msg);
        } else {
            std::cerr << text;
        }
    }

    void emitJsonResult(const std::string& status, const std::string& message) {
        if (!jsonProgressEnabled()) return;
        std::string o = openRecord("result");
        strOrNull(o, "status", status);
        if (!message.empty()) { o += ","; strOrNull(o, "message", message); }
        writeRecord(o);
    }

    void emitJsonOutput(const std::string& key, const std::string& value) {
        if (!jsonProgressEnabled()) return;
        std::string o = openRecord("output");
        strOrNull(o, "key", key); o += ",";
        strOrNull(o, "value", value);
        writeRecord(o);
    }

    void emitJsonWrite(const std::string& text) {
        if (!jsonProgressEnabled()) return;
        std::string o = openRecord("write");
        strOrNull(o, "text", text);
        writeRecord(o);
    }

    void emitJsonCacheHit(const std::string& artifact) {
        std::string o = openRecord("cache");
        strOrNull(o, "state", "hit");     o += ",";
        strOrNull(o, "artifact", artifact);
        writeRecord(o);
    }

    ProgressPhase::ProgressPhase(std::string phase, std::string label)
        : phase(std::move(phase)),
          label(std::move(label)),
          active(jsonProgressEnabled()),
          startedAt(std::chrono::steady_clock::now()) {
        if (active) emitJsonProgress(this->phase, "start", this->label);
    }

    ProgressPhase::~ProgressPhase() {
        if (!active) return;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt).count();
        // Runs during unwind too, so the IDE never leaves a phase spinning.
        emitJsonProgress(phase, "finish", label, static_cast<long long>(ms));
    }

    int levenshteinDistance(const std::string& a, const std::string& b) {
        // Two-row rolling DP; the swap keeps the row width at min(|a|, |b|) + 1.
        if (a.size() < b.size()) {
            return levenshteinDistance(b, a);
        }
        if (b.empty()) return static_cast<int>(a.size());

        std::vector<int> prev(b.size() + 1);
        std::vector<int> curr(b.size() + 1);
        for (std::size_t j = 0; j <= b.size(); ++j) {
            prev[j] = static_cast<int>(j);
        }
        for (std::size_t i = 1; i <= a.size(); ++i) {
            curr[0] = static_cast<int>(i);
            for (std::size_t j = 1; j <= b.size(); ++j) {
                int substCost = (a[i - 1] == b[j - 1]) ? 0 : 1;
                int del  = prev[j] + 1;
                int ins  = curr[j - 1] + 1;
                int subs = prev[j - 1] + substCost;
                curr[j] = std::min(std::min(del, ins), subs);
            }
            std::swap(prev, curr);
        }
        return prev[b.size()];
    }

    std::vector<std::string> pickSimilar(
            const std::string& target,
            const std::vector<std::string>& candidates,
            int maxDistance,
            std::size_t maxSuggestions) {
        // An exact match is not a typo, so it is no suggestion.
        std::vector<std::pair<int, std::string>> scored;
        scored.reserve(candidates.size());
        for (auto& c : candidates) {
            if (c == target) continue;
            int d = levenshteinDistance(target, c);
            if (d <= maxDistance) {
                scored.emplace_back(d, c);
            }
        }
        // Name breaks a distance tie, so the suggestion list is deterministic.
        std::sort(scored.begin(), scored.end(),
            [](const std::pair<int, std::string>& l,
               const std::pair<int, std::string>& r) {
                if (l.first != r.first) return l.first < r.first;
                return l.second < r.second;
            });
        std::vector<std::string> out;
        out.reserve(std::min(scored.size(), maxSuggestions));
        for (auto& s : scored) {
            if (out.size() >= maxSuggestions) break;
            out.push_back(s.second);
        }
        return out;
    }

    std::string formatDidYouMean(const std::vector<std::string>& suggestions) {
        if (suggestions.empty()) return "";
        if (suggestions.size() == 1) {
            return " did you mean `" + suggestions[0] + "`?";
        }
        std::string out = " did you mean one of: ";
        for (std::size_t i = 0; i < suggestions.size(); ++i) {
            if (i > 0) out += ", ";
            out += "`";
            out += suggestions[i];
            out += "`";
        }
        out += "?";
        return out;
    }

} // namespace cajeta
