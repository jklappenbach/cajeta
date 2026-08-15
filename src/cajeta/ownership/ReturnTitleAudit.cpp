#include "cajeta/ownership/ReturnTitleAudit.h"

#include <cstdlib>
#include <iostream>

namespace cajeta::ownership {

    const char* toString(TitleCarry carry) {
        switch (carry) {
            case TitleCarry::StaticTitle: return "static";
            case TitleCarry::RuntimeFlag: return "runtime";
        }
        return "?";
    }

    const char* toString(TitleVia via) {
        switch (via) {
            case TitleVia::CallRide:          return "call-ride";
            case TitleVia::FormalPassThrough: return "formal";
            case TitleVia::Move:              return "move";
            case TitleVia::ModeCarry:         return "mode-carry";
            case TitleVia::Flagged:           return "flagged";
            case TitleVia::Other:             return "other";
        }
        return "?";
    }

    namespace {
        // -1 = not yet read from the environment; 0/1 = the decided state.
        // A test's setEnabled wins over the environment for the rest of the
        // process, which is what makes the OFF-by-default control assertable.
        int g_enabled = -1;
        std::vector<ReturnTitleRecord>& sink() {
            static std::vector<ReturnTitleRecord> records;
            return records;
        }
        std::set<std::string>& seen() {
            static std::set<std::string> methods;
            return methods;
        }
    }  // namespace

    bool ReturnTitleAudit::enabled() {
        if (g_enabled < 0) {
            const char* env = std::getenv("CAJETA_AUDIT_RETURN_TITLES");
            g_enabled = (env && *env && std::string(env) != "0") ? 1 : 0;
        }
        return g_enabled == 1;
    }

    void ReturnTitleAudit::setEnabled(bool on) { g_enabled = on ? 1 : 0; }

    void ReturnTitleAudit::record(ReturnTitleRecord rec) {
        // One line per site, on the diagnostic channel, so a library build can
        // be harvested without linking against the compiler.
        std::cerr << "cajeta: note: [return-title] " << rec.className << "."
                  << rec.methodName << ":" << rec.line
                  << " carry=" << toString(rec.carry)
                  << " via=" << toString(rec.via)
                  << " returns=" << rec.returnType;
        if (rec.via == TitleVia::CallRide) {
            std::cerr << " callee="
                      << (rec.calleeKey.empty() ? "(closure)" : rec.calleeKey)
                      << " callee-owns=" << (rec.calleeOwned ? "1" : "0");
        }
        std::cerr << "\n";
        sink().push_back(std::move(rec));
    }

    const std::vector<ReturnTitleRecord>& ReturnTitleAudit::records() {
        return sink();
    }

    void ReturnTitleAudit::consider(const std::string& className,
                                    const std::string& methodName,
                                    const std::string& returnType) {
        std::string key = className + "." + methodName;
        if (!seen().insert(key).second) return;   // one line per method, ever
        std::cerr << "cajeta: note: [return-plain] " << key
                  << " returns=" << returnType << "\n";
    }

    const std::set<std::string>& ReturnTitleAudit::consideredMethods() {
        return seen();
    }

    void ReturnTitleAudit::clear() {
        sink().clear();
        seen().clear();
    }

}  // namespace cajeta::ownership
