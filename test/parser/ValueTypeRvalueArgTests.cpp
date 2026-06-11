//
// Regression: a value-type (stack struct) rvalue temporary passed as a
// function/method ARGUMENT must be materialized to a stable address before
// the call. Discovered via the tour's TimeDemo, where
// `mins.plus(Duration.ofSeconds(30))` (argument is an rvalue temporary)
// produced a garbage result (and segfaulted in --emit=exe), while the
// locals-only form `mins.plus(secs)` was correct.
//
// The receiver-rvalue case already worked (`Duration.ofMinutes(90).plus(x)`);
// the bug was specifically the rvalue ARGUMENT not being spilled to an alloca
// before its address was taken for the by-pointer struct ABI.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.time.Duration;\n"
           "public final class Vt {\n"
           "    public static int64 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

int64_t runJit(const std::string& body) {
    auto jit = CajetaJit::compile(makeSource(body), "test.Vt");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

} // namespace

// Baseline: argument is a named local (always worked).
TEST(ValueTypeRvalueArgTests, valueTypeArgViaLocal) {
    EXPECT_EQ(runJit(
        "Duration a = Duration.ofSeconds(5);\n"
        "Duration b = Duration.ofSeconds(3);\n"
        "Duration c = a.minus(b);\n"
        "return c.toSeconds();"), 2LL);
}

// The bug: argument is an rvalue temporary.
TEST(ValueTypeRvalueArgTests, valueTypeArgIsRvalueTemporary) {
    EXPECT_EQ(runJit(
        "Duration a = Duration.ofSeconds(5);\n"
        "return a.minus(Duration.ofSeconds(3)).toSeconds();"), 2LL);
}

// Both receiver and argument are rvalue temporaries (the original TimeDemo form).
TEST(ValueTypeRvalueArgTests, bothReceiverAndArgRvalue) {
    EXPECT_EQ(runJit(
        "return Duration.ofMinutes(90).plus(Duration.ofSeconds(30)).toSeconds();"),
        5430LL);
}

// Receiver is an rvalue, argument is a local (this already worked — pin it).
TEST(ValueTypeRvalueArgTests, receiverRvalueArgLocal) {
    EXPECT_EQ(runJit(
        "Duration b = Duration.ofSeconds(30);\n"
        "return Duration.ofMinutes(90).plus(b).toSeconds();"), 5430LL);
}
