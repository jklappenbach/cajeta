#pragma once

// The ride-through enumeration. A plain (non-`#`) return type should mean "borrow",
// but the caller reads the RUNTIME flag, and sanctioned shapes disagree: a tail call
// rides the callee's flag, a returned formal forwards, `return #x`, `return #= x`.

#include <set>
#include <string>
#include <vector>

namespace cajeta::ownership {

    // How the title is decided at this return.
    enum class TitleCarry {
        StaticTitle,   // constant 1 — this return always hands out a title
        RuntimeFlag,   // a value computed at run time; may be title or borrow
    };

    // Which mechanism put the title on the wire, one per assignment site in
    // ReturnStatement::generateCode.
    enum class TitleVia {
        CallRide,             // `return f()` — rides the callee's return flag
        FormalPassThrough,    // `return p` where p is a parameter
        Move,                 // `return #x`
        ModeCarry,            // `return #= x`
        Flagged,              // `return Cajeta.flagged(v, owned)`
        Other,                // a runtime flag from a shape not yet named
    };

    struct ReturnTitleRecord {
        std::string className;    // canonical, e.g. cajeta.codec.json.JsonObject
        std::string methodName;
        std::string returnType;   // declared (plain) return type, for the report
        int line = 0;             // source line of the `return`
        TitleCarry carry = TitleCarry::RuntimeFlag;
        TitleVia via = TitleVia::Other;

        // For a CallRide only: what is tail-called, and whether THAT method declares
        // `#`. Riding a plain callee only defers the decision, which is a fluent chain
        // and not a finding. Empty = a closure call, where nothing static decides.
        std::string calleeKey;
        bool calleeOwned = false;
    };

    const char* toString(TitleCarry carry);
    const char* toString(TitleVia via);

    // Collection sink. Not thread-safe: one module's codegen runs on one thread.
    class ReturnTitleAudit {
    public:
        // Reads CAJETA_AUDIT_RETURN_TITLES once, unless setEnabled overrode it.
        static bool enabled();
        static void setEnabled(bool on);

        static void record(ReturnTitleRecord rec);
        static const std::vector<ReturnTitleRecord>& records();

        // The DENOMINATOR: every plain-return, class-pointer-returning method codegen
        // reaches, titled or not, since a ride-through count means nothing without it.
        // Deduped by `class.method`, which compiles once per module that pulls it in.
        static void consider(const std::string& className,
                             const std::string& methodName,
                             const std::string& returnType);
        static const std::set<std::string>& consideredMethods();

        // A `#`-returning call result bound with PLAIN `=`, counted BEFORE the rule
        // requiring `#=` is written, so its blast radius is known first. Deduped by
        // receiving method and line.
        static void ownedBind(const std::string& calleeKey,
                              const std::string& inMethod, int line);
        static const std::set<std::string>& ownedBinds();

        static void clear();
    };

}  // namespace cajeta::ownership
