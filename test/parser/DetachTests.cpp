//
// `detach` semantics tests.
//
// detach is fire-and-forget — the detached call runs as a fiber on the
// carrier thread, the spawning method returns without waiting for it,
// and the Task wrapper is deliberately not scope-tracked (no
// scope_register) or drop-tracked (no drop_push for the Task struct).
// docs/stdlib/Concurrency.md § detach semantics has the spec; this suite covers:
//
//   - detach with no captures runs and run() returns immediately.
//   - detach with primitive captures works (primitives are pass-by-
//     value, no aliasing concern).
//   - detach with a #-transferred class captures works (explicit
//     ownership move).
//   - detach with a bare class-typed identifier (borrow capture) is
//     rejected at compile time with CAJETA_ERROR_DETACH_BORROW_CAPTURE.
//   - detach with a fresh `heap T(...)` capture works (auto-promotion).
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

// No-capture detach: the inner async body has no parameters, runs as a
// fiber, and run() returns 33 without waiting. The only thing this
// asserts at the source level is that no scope-anchored wait happens —
// run() returns the literal 33 promptly. This was the test the
// previous sync-passthrough detach also passed; under the new
// implementation we verify it still works post fiber wiring.
TEST(DetachTests, noCaptureDetachReturnsImmediately) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async void background() { return; }\n"
        "    public static int32 run() {\n"
        "        detach background();\n"
        "        return 33;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 33);
}

// Primitive-capture detach: int32 is pass-by-value, so capturing it
// for a detached call has no ownership implications. The detached
// body uses the value (consumes it once); the caller is free to keep
// using the same local afterward.
TEST(DetachTests, primitiveCaptureDetach) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async void use(int32 v) { return; }\n"
        "    public static int32 run() {\n"
        "        int32 x = 42;\n"
        "        detach use(x);\n"
        "        return x;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// #-transferred class capture: the explicit move makes the caller's
// reference moved-out (use-after-move check kicks in if read again).
// The detached body receives the heap pointer and owns it; the
// runtime never returns it to the caller.
TEST(DetachTests, hashCaptureDetachAccepted) {
    auto src =
        "package test;\n"
        "public class Payload {\n"
        "    public int32 n;\n"
        "    public Payload() { this.n = 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static async void consume(Payload p) { return; }\n"
        "    public static int32 run() {\n"
        "        Payload p = heap Payload();\n"
        "        detach consume(#p);\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Fresh allocator capture: a bare `heap T(...)` is auto-promoted in
// transfer position (MemoryModel.md § Borrow / transfer rules), so it
// counts as a transfer for detach's captures rule — no explicit `#`
// needed. Now testable since NewExpression sets its resolvedType
// during the resolve pass (previously left null, which made every
// inline `f(heap T())` call segfault in ReturnStatement on a null val).
TEST(DetachTests, freshAllocatorCaptureDetachAccepted) {
    auto src =
        "package test;\n"
        "public class Payload {\n"
        "    public int32 n;\n"
        "    public Payload() { this.n = 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static async void consume(Payload p) { return; }\n"
        "    public static int32 run() {\n"
        "        detach consume(heap Payload());\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Bare class-typed identifier without `#`: rejected. The detached
// fiber outlives the spawning scope, so the caller's `p` can't be
// safely borrowed into it — the local would drop while the fiber is
// still running. The error fires at the captures check before any
// IR is emitted for the inner call.
TEST(DetachTests, borrowCaptureRejected) {
    auto src =
        "package test;\n"
        "public class Payload {\n"
        "    public int32 n;\n"
        "    public Payload() { this.n = 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static async void consume(Payload p) { return; }\n"
        "    public static int32 run() {\n"
        "        Payload p = heap Payload();\n"
        "        detach consume(p);\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected cajeta::Exception (detach borrow capture) but compile succeeded";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_DETACH_BORROW_CAPTURE");
        EXPECT_NE(e.getMessage().find("Payload"), std::string::npos)
            << "exception message '" << e.getMessage()
            << "' did not name the offending captured type";
    } catch (std::exception& e) {
        FAIL() << "expected cajeta::Exception, got std::exception: " << e.what();
    }
}
