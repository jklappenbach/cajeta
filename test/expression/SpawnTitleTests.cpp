// The `spawn` expression's ownership stance. The spawn site pushes the Task's
// own drop entry, so the expression must hand back a BORROW -- and must SAY so,
// because the return flag is sticky TLS that the next `#=` binding reads.
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

// A `#`-returning call before the spawn, so the TLS flag holds 1 on entry.
const char* kCellSrc =
    "package test;\n"
    "public class Cell { public int32 n; public Cell(int32 n) { this.n = n; } }\n";

}  // namespace

// `#=` records the mode it is handed. The spawn site already owns the Task, so
// the mode handed over is a borrow and the binding must not arm a second owner.
TEST(SpawnTitleTests, sharpBoundSpawnDoesNotDoubleFree) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static async int32 work() { return 7; }\n"
        "    public static int32 run() {\n"
        "        Task<int32> t #= spawn work();\n"
        "        return await t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// The discriminating case for the whole stale-flag family: an owned call runs
// immediately before the spawn, leaving 1 in the TLS slot. A spawn that
// publishes no stance lets the binding inherit that 1 and free the Task twice.
TEST(SpawnTitleTests, spawnAfterAnOwnedCallStillLendsItsTask) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static async int32 work() { return 7; }\n"
        "    public static #Cell fresh() { return heap Cell(5); }\n"
        "    public static int32 run() {\n"
        "        Cell c #= D.fresh();\n"
        "        Task<int32> t #= spawn work();\n"
        "        return await t + c.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 12);
}

// Inside a scope block, where the frame joins the task, the same must hold.
TEST(SpawnTitleTests, sharpBoundSpawnInsideAScopeDoesNotDoubleFree) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static async int32 work() { return 7; }\n"
        "    public static int32 run() {\n"
        "        int32 got = 0;\n"
        "        scope {\n"
        "            Task<int32> t #= spawn work();\n"
        "            got = await t;\n"
        "        }\n"
        "        return got;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// The control: the plain binding was always safe and must stay that way.
TEST(SpawnTitleTests, plainBoundSpawnStillAwaits) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static async int32 work() { return 7; }\n"
        "    public static int32 run() {\n"
        "        Task<int32> t = spawn work();\n"
        "        return await t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// A spawned task whose value is never bound still runs and still frees once.
TEST(SpawnTitleTests, sharpBoundSpawnLeavesNoTitleForTheNextBinding) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static async int32 work() { return 7; }\n"
        "    public static Cell lend(Cell c) { return c; }\n"
        "    public static int32 run() {\n"
        "        Cell owner #= heap Cell(3);\n"
        "        Task<int32> t #= spawn work();\n"
        "        int32 got = await t;\n"
        "        Cell borrowed #= D.lend(owner);\n"
        "        return got + borrowed.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 10);
}
