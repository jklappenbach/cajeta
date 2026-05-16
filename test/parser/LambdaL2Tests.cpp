//
// L2-2 lambda tests — captures.
// Lambdas can now reference outer-scope primitives; each captured value is
// copied into a captures struct at the lambda creation site and unpacked
// inside the synthesized function via the implicit captures-ptr first arg.
// Capture-by-value semantics: subsequent mutation of the outer local does
// not affect what the lambda sees. See cajeta-docs/Lambdas.md.
//
// Out of scope (later L2 sub-slices):
//   - Heap captures by borrow
//   - `#name` transfer in captures
//   - Compile-error on writes to value-captured primitives
//   - Block-body lambdas
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

// Single primitive capture. The lambda body adds its param to a captured
// local; we exercise it after the outer local was set.
TEST(LambdaL2Tests, capturesPrimitiveByValue) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 base = 10;\n"
        "        (int32) -> int32 add = x -> x + base;\n"
        "        return add(5);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// Two captured primitives, ordered to verify the captures-struct field
// layout matches between the populate site (outer code) and the unpack
// site (lambda body).
TEST(LambdaL2Tests, capturesTwoPrimitives) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 a = 7;\n"
        "        int32 b = 3;\n"
        "        (int32) -> int32 fn = x -> x * a + b;\n"
        "        return fn(4);\n"  // 4*7 + 3 = 31
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 31);
}

// Capture-by-VALUE means the lambda sees the value at the moment of
// capture; later mutation of the outer local doesn't change what the
// lambda reads. Mirrors the spec example.
TEST(LambdaL2Tests, captureSnapshotsAtCreationTime) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 base = 10;\n"
        "        (int32) -> int32 add = x -> x + base;\n"
        "        base = 999;\n"
        "        return add(5);\n"  // captured base=10, not 999 → 15
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// Two lambdas in the same scope, each capturing a different local. Verifies
// captures structs are scoped per-lambda (not shared) and the call sites
// dispatch through the right closure record.
TEST(LambdaL2Tests, multipleLambdasCaptureIndependently) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 left = 100;\n"
        "        int32 right = 7;\n"
        "        (int32) -> int32 addLeft = x -> x + left;\n"
        "        (int32) -> int32 mulRight = x -> x * right;\n"
        "        return addLeft(1) + mulRight(2);\n"  // 101 + 14 = 115
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 115);
}

// Param shadows an outer name with the same identifier. The captured
// binding shouldn't be created in the first place — the body's `base`
// resolves to the lambda parameter.
TEST(LambdaL2Tests, paramShadowsOuterName) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 base = 999;\n"
        "        (int32) -> int32 fn = base -> base + 1;\n"
        "        return fn(4);\n"  // resolves to param, not outer
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// L2-3: heap capture by borrow. A String local (pointer alias) flows into
// the captures struct as a `ptr`; inside the lambda, the receiver dispatch
// path treats it as a String exactly as if it were a regular local.
TEST(LambdaL2Tests, capturesStringByBorrow) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"hello\";\n"
        "        (int32) -> int32 fn = n -> n + (int32) s.length();\n"
        "        return fn(2);\n"  // 2 + 5 = 7
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// L2-3: heap capture by borrow on an array. arr.count() reads the array
// header through the captured pointer.
TEST(LambdaL2Tests, capturesArrayByBorrow) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] arr = new int32[7];\n"
        "        () -> int64 fn = () -> arr.count();\n"
        "        return (int32) fn();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Borrow semantics: the captures struct stores the heap pointer at the
// capture moment. Mutating the heap object through the original local
// after the lambda is created is visible to the lambda — both reference
// the same heap memory. (Rebinding the outer slot to a *new* heap object
// would NOT be visible, but rebinding `arr` would be a fresh assignment
// that L2-3 doesn't exercise.)
TEST(LambdaL2Tests, capturedHeapMutationVisible) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] arr = new int32[3];\n"
        "        arr[0] = 10;\n"
        "        (int32) -> int32 read = i -> arr[i];\n"
        "        arr[0] = 99;\n"
        "        return read(0);\n"  // sees 99 (shared heap)
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// Mixed: capture one primitive and one heap value in the same lambda.
// Verifies the captures struct's mixed-field layout (i32 + ptr) round-
// trips correctly.
TEST(LambdaL2Tests, mixedPrimitiveAndHeapCaptures) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 bias = 100;\n"
        "        int32[] arr = new int32[5];\n"
        "        (int32) -> int32 fn = i -> arr[i] + bias;\n"
        "        return fn(0);\n"  // 0 + 100 = 100
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 100);
}

// L2-4: block-body lambda with an explicit return statement.
TEST(LambdaL2Tests, blockBodyExplicitReturn) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        () -> int32 f = () -> { return 42; };\n"
        "        return f();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// L2-4 + captures: block body that reads a captured primitive and a
// captured heap value into locals, combines them, returns the local.
TEST(LambdaL2Tests, blockBodyWithCapturesAndLocals) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 bias = 10;\n"
        "        int32[] arr = new int32[3];\n"
        "        arr[0] = 5;\n"
        "        (int32) -> int32 fn = i -> {\n"
        "            int32 v = arr[i];\n"
        "            int32 r = v + bias;\n"
        "            return r;\n"
        "        };\n"
        "        return fn(0);\n"  // v=5, r=5+10=15
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// Direct heap capture read inside a block-body lambda (sidesteps the
// StackField/ArrayIndex pre-existing gap by combining the read into the
// return expression). Confirms heap captures still flow through the
// captures struct under the block-body codegen path.
TEST(LambdaL2Tests, blockBodyDirectReturnHeapCap) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 bias = 10;\n"
        "        int32[] arr = new int32[3];\n"
        "        arr[0] = 5;\n"
        "        (int32) -> int32 fn = i -> {\n"
        "            return arr[i] + bias;\n"
        "        };\n"
        "        return fn(0);\n"  // 5 + 10 = 15
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// L2-5: writing to a value-captured primitive must be rejected at
// compile time. The lambda would silently mutate a private copy of the
// captured value — that's a footgun the language pins down per Rule 5
// in cajeta-docs/Lambdas.md. The exception carries the offending name
// so error messages stay actionable.
TEST(LambdaL2Tests, writingValueCaptureIsCompileError) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 counter = 0;\n"
        "        () -> int32 fn = () -> {\n"
        "            counter = counter + 1;\n"
        "            return counter;\n"
        "        };\n"
        "        return fn();\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected lambda value-capture write to throw";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_TYPE");
        EXPECT_NE(e.getMessage().find("counter"), std::string::npos);
        EXPECT_NE(e.getMessage().find("captured by value"), std::string::npos);
    }
}

// L2-5: compound assignment (`+=`) also counts as a write to the value
// capture. The detection key is `BinaryOpExpression::isAssignment()`,
// which is true for every assigning form.
TEST(LambdaL2Tests, compoundAssignToValueCaptureIsCompileError) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 acc = 0;\n"
        "        (int32) -> int32 fn = n -> {\n"
        "            acc += n;\n"
        "            return acc;\n"
        "        };\n"
        "        return fn(5);\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected compound-assign to value capture to throw";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_TYPE");
        EXPECT_NE(e.getMessage().find("acc"), std::string::npos);
    }
}

// Negative case: assigning a non-captured local (declared inside the
// lambda block) is fine — the check is name-based on the value-captured
// set, so a local with no shadow on the outside isn't affected.
TEST(LambdaL2Tests, writingLocalIsNotCaptureError) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 base = 10;\n"
        "        () -> int32 fn = () -> {\n"
        "            int32 local = base;\n"
        "            local = local + 5;\n"  // writing to local, not capture
        "            return local;\n"
        "        };\n"
        "        return fn();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// L2-4: block-body with branching control flow. Both arms return.
TEST(LambdaL2Tests, blockBodyWithControlFlow) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        (int32) -> int32 absish = x -> {\n"
        "            if (x < 0) {\n"
        "                return 0 - x;\n"
        "            }\n"
        "            return x;\n"
        "        };\n"
        "        return absish(0 - 7) + absish(3);\n"  // 7 + 3 = 10
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 10);
}
