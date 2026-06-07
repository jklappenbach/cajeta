//
// Tasks.runBlocking — sync→async bridge. The runtime's
// __cajeta_task_wait already falls through to a condvar wait when the
// caller has no current fiber, so a plain non-async main can drive
// async work via this helper without itself being async. v1 surface
// covers primitive / value-return R via the spawn-of-lambda ABI; the
// heap-return companion (() -> #R body) is intentionally not yet
// shipped — heap-return callers keep using the explicit
// `await spawn body()` pattern until then.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

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

// Plain non-async run() calls runBlocking with a no-capture lambda
// returning a primitive. Mirrors the v1 intent: a sync entry point that
// drives a one-shot async body and reads the result.
TEST(RunBlockingTests, primitiveBodyNoCaptures) {
    auto src =
        "package test;\n"
        "import cajeta.threading.Tasks;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        () -> int32 body = () -> { return 42; };\n"
        "        return Tasks.runBlocking<int32>(body);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Body itself drives a nested spawn-await — proves runBlocking's body
// is a real fiber context (nested awaits park/wake just like any other
// async body), not just a synchronous call dressed up as one.
TEST(RunBlockingTests, bodyDrivesNestedSpawn) {
    auto src =
        "package test;\n"
        "import cajeta.threading.Tasks;\n"
        "public final class D {\n"
        "    public static async int32 leaf() {\n"
        "        return 21;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        () -> int32 body = () -> {\n"
        "            int32 a = await spawn leaf();\n"
        "            int32 b = await spawn leaf();\n"
        "            return a + b;\n"
        "        };\n"
        "        return Tasks.runBlocking<int32>(body);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}
