//
// L3 lambda tests — capture-time `#name` transfer + lifetime.
//
// L3-1 (this file's initial slice): syntax + use-after-move marking.
// `#name` inside a lambda body marks the named outer binding as a
// transfer capture — ownership conceptually moves into the closure and
// subsequent reads of the outer name surface as use-after-move at
// compile time. Drop-chain transfer (the closure becoming the actual
// owner that frees at its own drop) is L3-3; for now the outer's drop
// still fires at scope exit, which is safe as long as the closure
// doesn't escape its declaring scope (L3-2's enforcement).
//
// Out of scope (later L3 sub-slices):
//   - L3-2: lifetime/escape check — closure returning past its borrows
//   - L3-3: closure drop-chain ownership (free at closure drop, not at
//           the original binding's scope exit)
//   - `#this` transfer (Rule 4) — needs `this` capture support first
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

} // namespace

// `#arr` inside the lambda body transfers ownership of the heap value
// into the closure. The body itself uses the captured pointer to call a
// method; within scope, before the original binding's drop fires, the
// pointer is still valid.
TEST(LambdaL3Tests, transferCaptureRunsWithinScope) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] arr = new int32[5];\n"
        "        () -> int64 fn = () -> #arr.size();\n"
        "        return (int32) fn();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// Outer use of the transferred name AFTER the lambda is created surfaces
// as use-after-move at compile time. This is what makes `#` meaningful:
// the static check catches a use that would have aliased the moved-out
// owner.
TEST(LambdaL3Tests, outerUseAfterTransferIsCompileError) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] arr = new int32[3];\n"
        "        () -> int64 fn = () -> #arr.size();\n"
        "        int64 size = arr.size();\n"  // use-after-move
        "        return (int32) size;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected use-after-move on outer read after transfer";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_USE_AFTER_MOVE");
        EXPECT_NE(e.getMessage().find("arr"), std::string::npos);
    }
}

// `#name` only triggers transfer mode for heap values. On a primitive
// (Rule 1 says primitives capture by value, no exceptions), the `#`
// marker is a no-op for capture categorization — the primitive copies
// into the captures struct as usual, and the outer binding stays
// readable. Documents the deliberate Rule-1-wins behaviour.
TEST(LambdaL3Tests, transferOnPrimitiveIsNoOp) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 base = 10;\n"
        "        (int32) -> int32 fn = x -> x + #base;\n"
        "        int32 stillReadable = base;\n"  // not moved
        "        return fn(5) + stillReadable;\n"  // 15 + 10 = 25
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 25);
}

// Mixed: one transfer + one borrow in the same lambda. The transfer
// name is marked moved; the borrow name stays readable.
TEST(LambdaL3Tests, transferAndBorrowCoexist) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] taken = new int32[2];\n"
        "        int32[] kept = new int32[7];\n"
        "        () -> int64 fn = () -> #taken.size() + kept.size();\n"
        "        int64 keptSize = kept.size();\n"  // still readable
        "        return (int32) fn() + (int32) keptSize;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2 + 7 + 7);
}
