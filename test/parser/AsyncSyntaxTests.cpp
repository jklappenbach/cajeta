//
// Sync-lowering MVP for the structured-concurrency keywords (ThreadModel.md):
// async / await / spawn / scope / detach. With no real scheduler yet, every
// spawn runs inline and finishes before the surrounding code continues —
// await/spawn collapse to direct calls, scope is just `{ ... }`, detach
// evaluates-and-discards. These tests prove the grammar and AST plumbing are
// in place end-to-end; the scheduler / Task<T> wrapping / state-machine
// lowering land in future phases without changing the surface syntax.
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

// `async` modifier parses on a method; the body codegens as a regular
// function. Calling it directly returns the inner value.
TEST(AsyncSyntaxTests, asyncMethodCallableDirectly) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 fortyTwo() { return 42; }\n"
        "    public static int32 run() { return fortyTwo(); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// `await` parses and passes the inner value through unchanged.
TEST(AsyncSyntaxTests, awaitPassesThroughInnerValue) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 produce() { return 7; }\n"
        "    public static int32 run() { return await produce(); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// `spawn` parses and runs the call inline (sync lowering).
TEST(AsyncSyntaxTests, spawnRunsCallInline) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 compute() { return 11; }\n"
        "    public static int32 run() { return spawn compute(); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}

// `await spawn` together is the canonical pattern; sync MVP returns the
// inner value unchanged through both layers.
TEST(AsyncSyntaxTests, awaitSpawnReturnsInnerValue) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 compute() { return 99; }\n"
        "    public static int32 run() { return await spawn compute(); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// `scope { ... }` is a statement that owns its contents. In the sync MVP
// it's just a block — locals declared inside drop at the closing `}`,
// same as any block. Verifies the parser routes SCOPE through to the
// block grammar correctly.
TEST(AsyncSyntaxTests, scopeBlockExecutesContents) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 compute() { return 5; }\n"
        "    public static int32 run() {\n"
        "        int32 r = 0;\n"
        "        scope {\n"
        "            r = await spawn compute();\n"
        "        }\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// `detach expr` parses and evaluates the inner expression for its side
// effects, returning no value to the surrounding context. The MVP runs
// it inline. Verifies the detach grammar + dispatch + codegen path.
TEST(AsyncSyntaxTests, detachExecutesAndDiscards) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async void noop() { return; }\n"
        "    public static int32 run() {\n"
        "        detach noop();\n"
        "        return 33;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 33);
}
