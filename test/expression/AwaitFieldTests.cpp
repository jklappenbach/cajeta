// `await` on a Task read from a field or a static, not a local. The operand
// then lowers to the slot's address (a GEP or a global), which holds the task
// pointer; await must load through it, as it does a local's alloca. On v0.52.0
// it used the slot itself as the task, so the value read back was garbage and a
// task that threw faulted the awaiter (cajeta-cabra's host shutdown, 2026-11-07).
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

const char* kServerSrc =
    "package test;\n"
    "import cajeta.concurrent.Fiber;\n"
    "import cajeta.time.Duration;\n"
    "public final class Server {\n"
    "    public Task<int32> t;\n"
    "    public static Task<int32> shared;\n"
    "    public int32 mode;\n"
    "    public Server(int32 mode) { this.mode = mode; return; }\n"
    "    public static async int32 work(Server s) {\n"
    "        Fiber.sleep(Duration.ofMillis(5L));\n"
    "        if (s.mode == 1) { throw heap Exception(\"from the task\"); }\n"
    "        return 7;\n"
    "    }\n"
    "    public int32 sameFrame() {\n"
    "        Task<int32> t = spawn work(this);\n"
    "        this.t #= t;\n"
    "        return await this.t;\n"
    "    }\n"
    "    public int32 bareName() {\n"
    "        Task<int32> u = spawn work(this);\n"
    "        this.t #= u;\n"
    "        return await t;\n"
    "    }\n"
    "    public int32 viaStatic() {\n"
    "        Task<int32> u = spawn work(this);\n"
    "        Server.shared #= u;\n"
    "        return await Server.shared;\n"
    "    }\n"
    "    public void start() {\n"
    "        Task<int32> u = spawn work(this);\n"
    "        this.t #= u;\n"
    "        return;\n"
    "    }\n"
    "    public int32 stop() {\n"
    "        return await this.t;\n"
    "    }\n"
    "    public int32 viaLocal() {\n"
    "        Task<int32> u = spawn work(this);\n"
    "        this.t #= u;\n"
    "        Task<int32> v = this.t;\n"
    "        return await v;\n"
    "    }\n"
    "}\n";

}  // namespace

// FIRES on v0.52.0: the explicit `this.t` read.
TEST(AwaitFieldTests, awaitOnAFieldReadReturnsTheTasksValue) {
    std::string src = std::string(kServerSrc) +
        "final class D {\n"
        "    public static int32 run() {\n"
        "        Server s = heap Server(0);\n"
        "        return s.sameFrame();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// FIRES: a bare name that resolves to the field is the same slot read.
TEST(AwaitFieldTests, awaitOnABareFieldNameReturnsTheTasksValue) {
    std::string src = std::string(kServerSrc) +
        "final class D {\n"
        "    public static int32 run() {\n"
        "        Server s = heap Server(0);\n"
        "        return s.bareName();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// FIRES: a static Task is a global slot.
TEST(AwaitFieldTests, awaitOnAStaticReturnsTheTasksValue) {
    std::string src = std::string(kServerSrc) +
        "final class D {\n"
        "    public static int32 run() {\n"
        "        Server s = heap Server(0);\n"
        "        return s.viaStatic();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// A task that throws, awaited through a field, re-raises into the awaiter's
// catch. FIRES on v0.52.0 as a SIGSEGV.
TEST(AwaitFieldTests, aThrowAwaitedThroughAFieldIsCaught) {
    std::string src = std::string(kServerSrc) +
        "final class D {\n"
        "    public static int32 run() {\n"
        "        Server s = heap Server(1);\n"
        "        int32 got = 0;\n"
        "        try {\n"
        "            got = s.sameFrame();\n"
        "        } catch (Exception e) {\n"
        "            got = 0 - 1;\n"
        "        }\n"
        "        return got;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), -1);
}

// A task kept in a field by one method and awaited by another: the shape of a
// server that starts in one call and joins in its shutdown.
TEST(AwaitFieldTests, aTaskKeptByOneMethodIsAwaitedByAnother) {
    std::string src = std::string(kServerSrc) +
        "final class D {\n"
        "    public static int32 run() {\n"
        "        Server s = heap Server(0);\n"
        "        s.start();\n"
        "        return s.stop();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// The control: a local copy of the field always awaited correctly.
TEST(AwaitFieldTests, awaitOnALocalCopyOfTheFieldStillWorks) {
    std::string src = std::string(kServerSrc) +
        "final class D {\n"
        "    public static int32 run() {\n"
        "        Server s = heap Server(0);\n"
        "        return s.viaLocal();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}
