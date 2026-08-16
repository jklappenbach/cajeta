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
// The JIT links its own copy of the runtime bitcode, so the capture flag
// exists twice: once in the host process (cajeta_test's linked runtime) and
// once inside the JIT. `__cajeta_trace_record` and the flag share a TU, so
// JIT'd throws read the JIT copy — that is the copy under test. Reading the
// host copy here would assert on state no JIT'd throw ever consults.
static int jitCaptureFlag(cajeta_test::CajetaJit* jit) {
    auto get = reinterpret_cast<int (*)()>(
        jit->lookupRawSymbol("__cajeta_get_stack_trace_capture"));
    EXPECT_NE(get, nullptr) << "capture getter unresolved in JIT";
    return get ? get() : -1;
}


// ExceptionReview 3.7: the compiler must EMIT the wiring that carries
// CompilerFlags.stackTraceCapture into the runtime — a module global ctor
// calling __cajeta_set_stack_trace_capture. Before this, --stack-trace-capture
// and every mode default were inert: main.cpp parsed the flag, nothing
// consumed it, and the runtime stayed at its enabled-by-default static. The
// two JIT tests below only passed because JitTestHelper poked the runtime
// setter directly, masking the missing codegen.
TEST(StackTraceCaptureTests, compilerEmitsCaptureCtorWhenFlagOff) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    CajetaJit::Options opts;
    opts.captureIr = true;
    opts.stackTraceCaptureEnabled = false;
    auto jit = CajetaJit::compile(src, "test.D", opts);
    const std::string& ir = jit->getModuleIr();
    EXPECT_NE(ir.find("__cajeta_init_stack_trace_capture"), std::string::npos)
        << "no capture-init ctor emitted";
    EXPECT_NE(ir.find("@__cajeta_set_stack_trace_capture(i32 0)"), std::string::npos)
        << "capture ctor did not pass the disabled flag";
}

TEST(StackTraceCaptureTests, compilerEmitsCaptureCtorWhenFlagOn) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    CajetaJit::Options opts;
    opts.captureIr = true;
    opts.stackTraceCaptureEnabled = true;
    auto jit = CajetaJit::compile(src, "test.D", opts);
    const std::string& ir = jit->getModuleIr();
    EXPECT_NE(ir.find("@__cajeta_set_stack_trace_capture(i32 1)"), std::string::npos)
        << "capture ctor did not pass the enabled flag";
}

// JIT integration: with capture DISABLED via Options. The emitted module
// ctor calls __cajeta_set_stack_trace_capture(0) at jit->initialize().
// Throw still works
// — the throwable surfaces normally, just no native frames are
// recorded. The side-table doesn't grow; we can't directly assert on
// it (it's static), but we can confirm the flag is off and the throw
// + catch flow still completes.
