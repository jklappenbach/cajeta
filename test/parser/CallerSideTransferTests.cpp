//
// Phase 1 of #68 (docs/stdlib/OwnershipTransfer.md): caller-side
// `#x` syntax. The caller can now explicitly mark an argument as a
// transfer at the call site, independent of whether the callee's formal
// is `#T`-marked. The codegen-side rule:
//
//   - If the callee's formal is `#T` (existing path), drop the caller's
//     entry. The same behavior as before.
//   - If the argument is `#x` (new path), drop the caller's entry, even
//     when the callee's formal is plain `T`.
//
// Either suffices; either acknowledges that ownership is leaving the
// caller. Phase 2 will tighten the model by rejecting (#T-formal, plain-x);
// Phase 1 stays purely additive — every existing call site keeps working.
//
// The probe matrix covers all four cells:
//
//   | Callee formal | Caller writes | Behavior     |
//   |---------------|---------------|--------------|
//   | T param       | x             | borrow       |
//   | T param       | #x            | transfer     |
//   | #T param      | x             | transfer (P1)|
//   | #T param      | #x            | transfer     |
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// `Holder` takes a class-typed param and stores it in a field. The
// signature varies between the two test halves: `Holder(Pair p)` vs
// `Holder(#Pair p)`.
constexpr const char* kPairAndHolderBorrow =
    "package test;\n"
    "public class Pair {\n"
    "    public int32 x;\n"
    "    public Pair(int32 x) { this.x = x; }\n"
    "}\n"
    "public class Holder {\n"
    "    public Pair p;\n"
    "    public Holder(Pair p) { this.p = p; }\n"
    "}\n";

constexpr const char* kPairAndHolderTransfer =
    "package test;\n"
    "public class Pair {\n"
    "    public int32 x;\n"
    "    public Pair(int32 x) { this.x = x; }\n"
    "}\n"
    "public class Holder {\n"
    "    public Pair p;\n"
    "    public Holder(#Pair p) { this.p = p; }\n"
    "}\n";

} // namespace

// (T, #x): plain-T formal, caller writes `#x`. Today this is the failing
// case pre-Phase-1 — the caller's drop stays active while the Holder's
// drop walks the same Pair, double-free. With caller-side `#x`, the
// caller's drop deactivates, Holder becomes the sole owner.
TEST(CallerSideTransferTests, plainFormalCallerSideTransferNoDoubleFree) {
    std::string src = std::string(kPairAndHolderBorrow) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Pair p = heap Pair(42);\n"
        "        Holder h = heap Holder(#p);\n"
        "        return h.p.x;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// (#T, x): callee formal is `#T`, caller writes plain `x` of a class-
// typed local with an active drop entry. Phase 2 rejects this at the
// call site — the callee is contractually demanding transfer, and a
// silent ambient transfer (Phase 1 behavior) would hide the
// double-free hazard from the reader. The fix is to acknowledge the
// transfer with `#x` (covered by the test below).
TEST(CallerSideTransferTests, transferFormalPlainArgIsRejected) {
    std::string src = std::string(kPairAndHolderTransfer) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Pair p = heap Pair(7);\n"
        "        Holder h = heap Holder(p);\n"
        "        return h.p.x;\n"
        "    }\n"
        "}\n";
    EXPECT_THROW(runI32(src), cajeta::Exception);
}

// (#T, #x): both ends agree. The drop deactivates once (idempotent —
// the codegen-side check fires once regardless of which side asked).
TEST(CallerSideTransferTests, bothSidesAgreeTransfers) {
    std::string src = std::string(kPairAndHolderTransfer) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Pair p = heap Pair(99);\n"
        "        Holder h = heap Holder(#p);\n"
        "        return h.p.x;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// (T, #x) on a method call (not a ctor). Same shape: plain-`T` formal,
// caller writes `#x`. The drop should still deactivate via the
// MethodCallExpression.cpp transfer block (the parallel of
// CreatorRest.cpp).
TEST(CallerSideTransferTests, methodCallCallerSideTransferNoDoubleFree) {
    auto src =
        "package test;\n"
        "public class Pair {\n"
        "    public int32 x;\n"
        "    public Pair(int32 x) { this.x = x; }\n"
        "}\n"
        "public class Sink {\n"
        "    public Pair p;\n"
        "    public Sink() { this.p = null; }\n"
        "    public void take(Pair p) { this.p = p; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Pair p = heap Pair(13);\n"
        "        Sink s = heap Sink();\n"
        "        s.take(#p);\n"
        "        return s.p.x;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 13);
}

// Primitives shouldn't trigger any transfer — they don't have drop
// entries, so the deactivation path is a no-op. Verify the syntax is
// accepted without changing behavior. `#42` is rejected at the
// caller-side gate (REFERENCE only applies to IdentifierExpression
// args; a literal isn't an identifier, so no transfer fires — same
// as today's gate at the codegen side).
TEST(CallerSideTransferTests, primitiveArgCallerSideTransferIsNoOp) {
    auto src =
        "package test;\n"
        "public class Box {\n"
        "    public int32 v;\n"
        "    public Box(int32 v) { this.v = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 n = 5;\n"
        "        Box b = heap Box(#n);\n"  // `#` on int32 local: parse OK, codegen no-op
        "        return b.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}
