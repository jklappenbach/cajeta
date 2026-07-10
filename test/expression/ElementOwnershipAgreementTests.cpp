//
// element-ownership Unit 4B — call-site agreement with the instantiation's
// ownership mode (spec §3.1.3-4, §4.1.4; plan 4.1.1 / 4.1.4). The caller's
// transfer syntax must match the instantiation: `#` into a borrow-mode
// type-argument position is a compile error (the container never frees, so a
// transferred value is a guaranteed leak); a `#`-returning element extractor
// is callable only on an owning instantiation. Concrete (non-generic) classes
// keep the runtime moveMask contract and never trip these (§3.1.4).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Compile expecting an error; returns the message and (4.1.5) asserts the
// STABLE diagnostic code. The `--diag-format=json` rendering of any errorId
// as the `code` field is pinned by the existing DiagFormat tests (main.cpp
// routes every cajeta::Exception through emitJsonDiagnostic), so asserting
// the id here pins the full machine-readable surface.
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

// The dual-mode container under test: author-marked transfer mutator
// (put(#K)), read-only accessor (peek), explicit extractor (#K take).
const char* kCrateSrc =
    "package test;\n"
    "public class Elem { public int32 tag; public Elem(int32 t) { this.tag = t; } }\n"
    "public class Crate<K> {\n"
    "    public K store;\n"
    "    public void put(#K k) { this.store = k; }\n"
    "    public K peek() { return this.store; }\n"
    "    public #K take() { return this.store; }\n"
    "}\n";

} // namespace

// 4.1.1 — `#k` into a plain (borrow) type-argument position is a compile
// error whose message names both fixes (instantiate `<#...>`, or drop `#`).
TEST(ElementOwnershipAgreementTests, hashTransferIntoBorrowModeIsCompileError) {
    std::string src = std::string(kCrateSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Crate<Elem> scratch = heap Crate<Elem>();\n"
        "        Elem e = heap Elem(7);\n"
        "        scratch.put(#e);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    std::string msg = compileExpectError(src, "CAJETA_ERROR_ELEMENT_TRANSFER_MODE");
    EXPECT_NE(msg.find("borrow"), std::string::npos) << msg;
    EXPECT_NE(msg.find("#"), std::string::npos) << msg;
    EXPECT_NE(msg.find("Crate"), std::string::npos) << msg;
}

// Positive control — the same call under an owning instantiation compiles and
// runs (the transfer is the real `#T`-formal semantics).
TEST(ElementOwnershipAgreementTests, hashTransferIntoOwningModeCompiles) {
    std::string src = std::string(kCrateSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Crate<#Elem> owned = heap Crate<#Elem>();\n"
        "        Elem e = heap Elem(7);\n"
        "        owned.put(#e);\n"
        "        return owned.peek().tag;\n"  // 7
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// 4.1.4 — read-only accessors (plain K) stay borrows in BOTH modes: a
// borrow-mode Cache serves peek() with no friction.
TEST(ElementOwnershipAgreementTests, readOnlyAccessorWorksInBorrowMode) {
    std::string src = std::string(kCrateSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Crate<Elem> scratch = heap Crate<Elem>();\n"
        "        Elem e = heap Elem(9);\n"
        "        scratch.put(e);\n"           // borrow store, no transfer
        "        return scratch.peek().tag;\n" // 9
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 9);
}

// 4.1.4 — the `#K take()` extractor is valid only on an owning instantiation:
// on a borrow-mode Cache there is no owned element to hand out.
TEST(ElementOwnershipAgreementTests, extractorOnBorrowModeIsCompileError) {
    std::string src = std::string(kCrateSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Crate<Elem> scratch = heap Crate<Elem>();\n"
        "        Elem e = heap Elem(3);\n"
        "        scratch.put(e);\n"
        "        Elem got = scratch.take();\n"  // extractor on borrow mode
        "        return got.tag;\n"
        "    }\n"
        "}\n";
    std::string msg = compileExpectError(src, "CAJETA_ERROR_ELEMENT_EXTRACT_MODE");
    EXPECT_NE(msg.find("take"), std::string::npos) << msg;
    EXPECT_NE(msg.find("own"), std::string::npos) << msg;
}

// Positive control — the extractor COMPILES on an owning instantiation (the
// gate accepts it). Compile-only: the relinquish-at-teardown runtime semantics
// (container must not double-drop a taken element) are Unit 6/8 work.
TEST(ElementOwnershipAgreementTests, extractorOnOwningModeCompiles) {
    std::string src = std::string(kCrateSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Crate<#Elem> owned = heap Crate<#Elem>();\n"
        "        owned.put(heap Elem(5));\n"    // fresh construction satisfies owning
        "        Elem got = owned.take();\n"
        "        return got.tag;\n"
        "    }\n"
        "}\n";
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.D"));
}

// 4.1.2 (verification — shipped by #68 Phase 2, re-pinned here in the
// element-ownership shape): a plain owned local into an OWNING position is a
// compile error demanding explicitness (`#k` move, or `.clone()` copy) — the
// owning put(#K) formal is a real transfer position, and a silent handoff
// would leave two presumed owners.
TEST(ElementOwnershipAgreementTests, plainOwnedLocalIntoOwningPositionIsCompileError) {
    std::string src = std::string(kCrateSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Crate<#Elem> owned = heap Crate<#Elem>();\n"
        "        Elem e = heap Elem(2);\n"
        "        owned.put(e);\n"               // no transfer acknowledged
        "        return 0;\n"
        "    }\n"
        "}\n";
    std::string msg = compileExpectError(src, "CAJETA_ERROR_TRANSFER_REQUIRED");
    EXPECT_NE(msg.find("#e"), std::string::npos) << msg;   // names the fix
    EXPECT_NE(msg.find("ownership"), std::string::npos) << msg;
}

// 4.2.2 / 4.3.2 — a CONCRETE class keeps the runtime moveMask contract: `#e`
// into a plain formal on a non-generic class is legal (no type argument to
// agree with), unchanged by the parameterized checks.
TEST(ElementOwnershipAgreementTests, concreteMoveMaskClassUnaffected) {
    auto src = std::string(
        "package test;\n"
        "public class Elem { public int32 tag; public Elem(int32 t) { this.tag = t; } }\n"
        "public class Bag {\n"
        "    public Elem it;\n"
        "    public void add(Elem e) { this.it = e; }\n"
        "    public int32 tagOf() { return this.it.tag; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Bag b = heap Bag();\n"
        "        Elem e = heap Elem(4);\n"
        "        b.add(#e);\n"                   // runtime moveMask transfer
        "        return b.tagOf();\n"            // 4
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 4);
}
