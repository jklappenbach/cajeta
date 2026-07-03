//
// ExceptionReview plan §5 (retrieval API) / §8 (tests): getMessage/getCause
// round-trip through throw->catch. RED until the accessors exist on
// cajeta.error.Throwable / Exception. The first test grounds the substrate
// (real object throw + public-field read) the API wraps.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {

// A test.S.run() harness returning int32 (Exception/Throwable are prelude types,
// no import needed — mirrors SwitchAndExceptionTests).
std::string src(const std::string& body) {
    return "package test;\n"
           "public final class S {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// Substrate: throwing a real heap Exception (not the legacy int path) and reading
// its public `message` field in the catch works. Grounds §1/§2 for the API.
TEST(ThrowableApi, objectThrowPublicFieldRead) {
    auto jit = CajetaJit::compile(src(
        "try {\n"
        "    throw heap Exception(\"disk full\");\n"
        "} catch (Exception e) {\n"
        "    String m = e.message;\n"
        "    return m.count();\n"
        "}\n"
        "return -1;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 9);  // "disk full" == 9 chars
}

// §5: getMessage() accessor returns the message (no raw struct-offset access).
TEST(ThrowableApi, getMessageRoundTrip) {
    auto jit = CajetaJit::compile(src(
        "try {\n"
        "    throw heap Exception(\"disk full\");\n"
        "} catch (Exception e) {\n"
        "    String m = e.getMessage();\n"
        "    return m.count();\n"
        "}\n"
        "return -1;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 9);
}

// §5: getCause() accessor returns the cause link (none => null) without throwing.
// A plain Exception("...") sets cause = 0, so getCause() is null and the guard
// returns 1.
TEST(ThrowableApi, getCauseNullWhenNoCause) {
    auto jit = CajetaJit::compile(src(
        "try {\n"
        "    throw heap Exception(\"no cause\");\n"
        "} catch (Exception e) {\n"
        "    Throwable c = e.getCause();\n"
        "    if (c == null) { return 1; }\n"
        "    return 0;\n"
        "}\n"
        "return -1;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// §5: printStackTrace() wraps __cajeta_print_trace via the object bridge and
// runs without crashing from user code.
TEST(ThrowableApi, printStackTraceRuns) {
    auto jit = CajetaJit::compile(src(
        "try {\n"
        "    throw heap Exception(\"boom\");\n"
        "} catch (Exception e) {\n"
        "    e.printStackTrace();\n"
        "    return 1;\n"
        "}\n"
        "return -1;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// §5: getStackTrace() returns non-empty frames on a capture-on (JIT default)
// Linux build. Acceptance #2: top frame is the throw site (non-empty here).
TEST(ThrowableApi, getStackTraceNonEmpty) {
    auto jit = CajetaJit::compile(src(
        "try {\n"
        "    throw heap Exception(\"boom\");\n"
        "} catch (Exception e) {\n"
        "    StackFrame[] f = e.getStackTrace();\n"
        "    return f.count();\n"
        "}\n"
        "return -1;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_GT(fn(), 0);
}

// §5: each frame carries a resolvable (non-zero) native address.
TEST(ThrowableApi, getStackTraceTopAddressNonZero) {
    auto jit = CajetaJit::compile(src(
        "try {\n"
        "    throw heap Exception(\"boom\");\n"
        "} catch (Exception e) {\n"
        "    StackFrame[] f = e.getStackTrace();\n"
        "    if (f.count() == 0) { return 0; }\n"
        "    StackFrame top = f[0];\n"
        "    if (top.nativeAddress != 0) { return 1; }\n"
        "    return 0;\n"
        "}\n"
        "return -1;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}
