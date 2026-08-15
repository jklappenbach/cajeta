#pragma once

// stdlib-ownership-convention Unit 8 (8.1.1) — the ride-through enumeration.
//
// A plain (non-`#`) return type is supposed to mean "borrow", but the return's
// runtime flag is what the caller actually reads, and the two disagree in
// several sanctioned shapes: a tail call rides the inner callee's flag out
// (SignatureAbiTests.tailCallThroughPlainReturnKeepsTitle), a returned formal
// forwards whatever the caller lent, `return #x` surrenders outright, and
// `return #= x` releases whatever title the frame happens to hold.
//
// Unit 8 asks whether the return-side signal still carries the ownership
// decision. That question turns on HOW MANY such sites exist, and this is the
// instrument that counts them — hooked at `ReturnStatement::generateCode`'s
// own `returnTitleFlag`, so it reports the compiler's decision rather than a
// re-derivation of it. A second classifier would be a second thing to be
// wrong (3.3.3: the static audit keys on shapes it recognises; the compiler
// sees every one).
//
// Off unless asked for: `CAJETA_AUDIT_RETURN_TITLES=1` in the environment, or
// `setEnabled(true)` from a test. Enabled, it also prints one grep-able note
// per site on stderr so a whole-library build can be harvested:
//
//   cajeta: note: [return-title] cajeta.codec.json.JsonObject.take:118 \
//       carry=runtime via=call-ride

#include <set>
#include <string>
#include <vector>

namespace cajeta::ownership {

    // How the title is decided at this return.
    enum class TitleCarry {
        StaticTitle,   // constant 1 — this return always hands out a title
        RuntimeFlag,   // a value computed at run time; may be title or borrow
    };

    // Which mechanism put the title on the wire. These mirror the assignment
    // sites in ReturnStatement::generateCode one-for-one.
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

        // For a CallRide only: what is being tail-called, and whether THAT
        // method declares `#`. The distinction decides how much a ride-through
        // site means. `return this.setStringOwned(#w, n)` rides a callee that
        // hands out a title through a plain signature — the `viaPlain` shape
        // Unit 8 is counting. `return this.key(bytes, n)` rides a callee that
        // is itself plain, so the decision is only deferred, which is what a
        // fluent builder chain looks like and is not the same finding.
        // Empty callee = a closure call (the `Stream.fold<R>` shape), where
        // the callback is a parameter and nothing static decides at all.
        std::string calleeKey;
        bool calleeOwned = false;
    };

    const char* toString(TitleCarry carry);
    const char* toString(TitleVia via);

    // Collection sink. Not thread-safe: codegen for one module runs on one
    // thread, and the harvest is a single build.
    class ReturnTitleAudit {
    public:
        // Reads CAJETA_AUDIT_RETURN_TITLES once, unless setEnabled overrode it.
        static bool enabled();
        static void setEnabled(bool on);

        static void record(ReturnTitleRecord rec);
        static const std::vector<ReturnTitleRecord>& records();

        // The DENOMINATOR. Every plain-return, class-pointer-returning method
        // codegen reaches is counted here, whether or not it carries a title —
        // "17 ride-through sites" answers nothing without "out of how many".
        // Deduped by `class.method`, since one method compiles once per module
        // that pulls it in. Emits `[return-plain]` so a build's stderr carries
        // both halves of the ratio.
        static void consider(const std::string& className,
                             const std::string& methodName,
                             const std::string& returnType);
        static const std::set<std::string>& consideredMethods();

        // 8.2.7 sizing — a `#`-returning call result bound with PLAIN `=`.
        // Spec §4.6 would require `#=` at these sites so an acquisition is
        // visible where the value is received; this counts them BEFORE the
        // rule is written, because 3.3.3's lesson is that an ownership check
        // whose blast radius is discovered by the gate costs a unit.
        // Deduped by receiving method + line.
        static void ownedBind(const std::string& calleeKey,
                              const std::string& inMethod, int line);
        static const std::set<std::string>& ownedBinds();

        static void clear();
    };

}  // namespace cajeta::ownership
