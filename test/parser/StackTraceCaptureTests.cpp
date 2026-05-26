// Tests for the --stack-trace-capture=on/off flag
// (CompilerModes.md § --stack-trace-capture).
//
// The runtime captures a native backtrace at every throw via
// __cajeta_trace_record. With this flag on (default), the side-table
// grows on each throw — the dump on uncaught-throw uses it. With the
// flag off, capture is a no-op and the dump emits nothing for the
// trace.
//
// Tests exercise the getter/setter, run a JIT program that throws
// and is caught (so the trace is recorded but not surfaced), and
// verify the flag's deterministic per-test state via JIT init.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

extern "C" {
    void __cajeta_set_stack_trace_capture(int enabled);
    int  __cajeta_get_stack_trace_capture(void);
}

namespace {

class StackTraceGuard {
public:
    StackTraceGuard() : saved(__cajeta_get_stack_trace_capture()) {}
    ~StackTraceGuard() { __cajeta_set_stack_trace_capture(saved); }
private:
    int saved;
};

} // namespace

// Setter round-trip.
TEST(StackTraceCaptureTests, setterRoundTrip) {
    StackTraceGuard guard;
    __cajeta_set_stack_trace_capture(0);
    EXPECT_EQ(__cajeta_get_stack_trace_capture(), 0);
    __cajeta_set_stack_trace_capture(1);
    EXPECT_EQ(__cajeta_get_stack_trace_capture(), 1);
    // Truthy normalizes.
    __cajeta_set_stack_trace_capture(42);
    EXPECT_EQ(__cajeta_get_stack_trace_capture(), 1);
    __cajeta_set_stack_trace_capture(0);
    EXPECT_EQ(__cajeta_get_stack_trace_capture(), 0);
}

// JIT integration: with capture ENABLED (the JIT default), a thrown +
// caught exception runs cleanly. We don't assert on trace contents
// (frame addresses are JIT-dependent) — the existing ErrorModelTests
// pin the catch path; here we just confirm the flag is on after JIT
// init.
TEST(StackTraceCaptureTests, jitDefaultEnablesCapture) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 result = -1;\n"
        "        try { throw 7; } catch (Exception e) { result = (int32) e; }\n"
        "        return result;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    EXPECT_EQ(__cajeta_get_stack_trace_capture(), 1);
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

// JIT integration: with capture DISABLED via Options. The JIT init
// hook calls __cajeta_set_stack_trace_capture(0). Throw still works
// — the throwable surfaces normally, just no native frames are
// recorded. The side-table doesn't grow; we can't directly assert on
// it (it's static), but we can confirm the flag is off and the throw
// + catch flow still completes.
TEST(StackTraceCaptureTests, jitOptionDisablesCapture) {
    StackTraceGuard guard;
    __cajeta_set_stack_trace_capture(1);  // start on to prove init flips it off

    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 result = -1;\n"
        "        try { throw 13; } catch (Exception e) { result = (int32) e; }\n"
        "        return result;\n"
        "    }\n"
        "}\n";
    CajetaJit::Options opts;
    opts.stackTraceCaptureEnabled = false;
    auto jit = CajetaJit::compile(src, "test.D", opts);
    EXPECT_EQ(__cajeta_get_stack_trace_capture(), 0);
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 13);
}
