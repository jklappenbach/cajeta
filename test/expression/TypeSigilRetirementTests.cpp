//
// title-tracking Unit 7 (plan 7.1.1 + 7.1.3) — retirement of `#` in TYPE
// position, spec §8.1. Ownership is per-call under rev 2 (the call site
// decides, the entry/flag records), so a type-position sigil is
// meaningless-at-best and can contradict the initializer:
//   - type ARGUMENTS:  `ArrayList<#String>`, `HashMap<int32,#Cell>`
//   - local DECLARATIONS: `#Cell x = ...` (approved 2026-07-11)
// Both get the SAME deprecation error naming the spec. Signature positions
// are unaffected: `#V` formals stay as the opt-in must-own edge (sub-fork
// A) and `#T` returns stay as the ownership-return marker.
//
// Red-first: at authoring time all three rejected shapes still compile.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

const char* kCellSrc =
    "package test;\n"
    "import cajeta.collection.ArrayList;\n"
    "import cajeta.collection.HashMap;\n"
    "public class Cell {\n"
    "    public int32 n;\n"
    "    public Cell(int32 nn) { this.n = nn; }\n"
    "}\n";

int32_t runI32(const std::string& src, const char* entryClass = "test.D") {
    auto jit = CajetaJit::compile(src, entryClass);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

std::string compileExpectError(const std::string& src,
                               const std::string& expectCode) {
    try {
        CajetaJit::compile(src, "test.D");
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), expectCode);
        return e.getMessage();
    } catch (const std::exception& e) {
        return e.what();
    }
    ADD_FAILURE() << "expected a compile error";
    return "";
}

}  // namespace

// 7.1.1 — `#` on a type ARGUMENT is retired: containers are dual-capable
// per call, there is no owning instantiation to declare.
TEST(TypeSigilRetirementTests, typeArgumentSigilRejected) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<#Cell> a = heap ArrayList<#Cell>();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    std::string msg = compileExpectError(src,
        "CAJETA_ERROR_TYPE_TRANSFER_RETIRED");
    EXPECT_NE(msg.find("title-tracking"), std::string::npos)
        << "error must name the superseding spec; got: " << msg;
}

// 7.1.1 — same for a `#` on one of several arguments.
TEST(TypeSigilRetirementTests, mapValueTypeArgumentSigilRejected) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<int32, #Cell> m = heap HashMap<int32, #Cell>();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_TYPE_TRANSFER_RETIRED");
}

// 7.1.3 — `#Type` on a LOCAL declaration is retired: a local's role comes
// from its initializer shape.
TEST(TypeSigilRetirementTests, localDeclarationSigilRejected) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        #Cell x = heap Cell(4);\n"
        "        return x.n;\n"
        "    }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_TYPE_TRANSFER_RETIRED");
}

// Sub-fork A retained — a `#V` FORMAL (must-own edge) still compiles and
// consumes: the callee owns, the caller's `#` is mandatory and legal.
TEST(TypeSigilRetirementTests, signatureSharpFormalStillLegal) {
    std::string src = std::string(kCellSrc) +
        "public class Sink {\n"
        "    public Cell held;\n"
        "    public void keep(#Cell v) { this.held = #v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Sink s = heap Sink();\n"
        "        s.keep(#heap Cell(6));\n"
        "        return s.held.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
}

// Retained — a `#T` RETURN (ownership-return marker) still compiles; the
// caller's receiving local owns and drops the result.
TEST(TypeSigilRetirementTests, signatureSharpReturnStillLegal) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static #Cell make() { return #heap Cell(8); }\n"
        "    public static int32 run() {\n"
        "        Cell c = make();\n"
        "        return c.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 8);
}
