//
// diagnostic-exceptions plan Unit 1: Throwable.toJson() emits the unified
// --diag-format=json schema (superset of {severity,code,message,...}). RED
// until toDiagnostic()/toJson() exist. Content is asserted in-program via
// String.contains so a returned Cajeta String can be checked from the JIT.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <map>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string src(const std::string& body) {
    return "package test;\n"
           "public final class S {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// 1.1.1: toJson() yields an object carrying at least code + message; the message
// text round-trips.

// 1.1.2: code defaults to the canonical type name when no explicit code is set.

// optional-borrow-ownership 4.1.1 — toJson() used to SIGSEGV on a chain of TWO
// causes. Exception.getCause() wrapped its borrowed `cause` field in Optional's
// owning `#T` ctor, so the loop-scoped `Optional<Throwable> nxt` in toJson()'s
// walk dropped each iteration and freed the link the walk just aliased. Depth 0
// and 1 passed (the drop landed after last use), which is why
// causeChainSerializesNestedCauses below — exactly one cause deep — never caught
// it. Fixed by making Optional's `#T` ctor parameter mode-dependent.
TEST(DiagnosticJson, causeChainDepth2DoesNotCrash) {
    auto jit = CajetaJit::compile(src(
        "try {\n"
        "    throw heap Exception(\"L1\", heap Exception(\"L2\", heap Exception(\"L3\")));\n"
        "} catch (Exception e) {\n"
        "    String j #= e.toJson();\n"
        "    boolean ok = j.contains(\"L1\") && j.contains(\"L2\") && j.contains(\"L3\");\n"
        "    if (ok) { return 1; }\n"
        "    return 0;\n"
        "}\n"
        "return -1;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// optional-borrow-ownership 4.1.2 — the walk is depth-independent, not merely
// one deeper than it used to be.
TEST(DiagnosticJson, causeChainDepth5Serializes) {
    auto jit = CajetaJit::compile(src(
        "try {\n"
        "    throw heap Exception(\"L1\", heap Exception(\"L2\", heap Exception(\"L3\",\n"
        "        heap Exception(\"L4\", heap Exception(\"L5\")))));\n"
        "} catch (Exception e) {\n"
        "    String j #= e.toJson();\n"
        "    boolean ok = j.contains(\"L2\") && j.contains(\"L3\")\n"
        "              && j.contains(\"L4\") && j.contains(\"L5\");\n"
        "    if (ok) { return 1; }\n"
        "    return 0;\n"
        "}\n"
        "return -1;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// 1.1.3: nested causes serialize as a causeChain array (outer→inner), each with
// its own message. Exactly ONE cause deep — kept as the boundary case that used
// to be the only nested-cause coverage. See causeChainDepth2DoesNotCrash above.

// 1.1.4: the current getStackTrace() output serializes into a frames array
// (address-form at this stage). Capture is on by default in the JIT.

// 2.1.1: a user exception's public fields serialize into `fields` with correct
// names + typed values (via reflection), with no per-type serialization code.

// 2.1.3: category affordances default false and serialize; a subclass override
// flips them. 2.1.4: remediation round-trips when hint()/docUrl() are set, and
// is absent otherwise.


// 2.1.2: @DiagnosticCode("...") on a type sets its JSON `code`; without it, code
// stays the canonical type name (default covered by codeDefaultsToTypeName).
