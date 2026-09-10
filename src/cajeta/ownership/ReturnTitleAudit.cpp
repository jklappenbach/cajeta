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
        // -1 = not yet read from the environment; a test's setEnabled then wins for the process.
        int g_enabled = -1;
        std::vector<ReturnTitleRecord>& sink() {
            static std::vector<ReturnTitleRecord> records;
            return records;
        }
        std::set<std::string>& seen() {
            static std::set<std::string> methods;
            return methods;
        }
        std::set<std::string>& binds() {
            static std::set<std::string> sites;
            return sites;
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
        // One line per site on the diagnostic channel. Line 0 means "no trustworthy
        // line" — a monomorphization counts into a synthesized buffer — so a marker prints.
        std::cerr << "cajeta: note: [return-title] " << rec.className << "."
                  << rec.methodName;
        if (rec.line > 0) std::cerr << ":" << rec.line;
        else std::cerr << ":(instantiation)";
        std::cerr << " carry=" << toString(rec.carry)
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

    void ReturnTitleAudit::ownedBind(const std::string& calleeKey,
                                     const std::string& inMethod, int line) {
        // `line <= 0` means the caller could not vouch for it, so a marker prints rather
        // than a wrong number; instantiation records dedupe per method+callee instead.
        const std::string where = line > 0 ? std::to_string(line)
                                           : std::string("(instantiation)");
        std::string key = inMethod + ":" + where + " <- " + calleeKey;
        if (!binds().insert(key).second) return;
        std::cerr << "cajeta: note: [owned-bind] " << inMethod << ":" << where
                  << " callee=" << calleeKey << "\n";
    }

    const std::set<std::string>& ReturnTitleAudit::ownedBinds() {
        return binds();
    }

    void ReturnTitleAudit::clear() {
        sink().clear();
        seen().clear();
        binds().clear();
    }

}  // namespace cajeta::ownership
