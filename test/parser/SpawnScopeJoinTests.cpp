//
// Discarded-spawn join placement (Concurrency.md § scope blocks).
//
// A statement-position `spawn f(...);` whose Task is never bound must be
// joined by the NEAREST ENCLOSING SCOPE FRAME — an explicit `scope { }`
// or the function-body implicit scope — NOT at the innermost lexical
// brace. The spec's own example depends on it:
//
//     scope { for (u : urls) { spawn fetch(u); } }   // concurrent, join at }
//
// Before 2026-07-20 the lowering wired the discarded Task into the
// innermost block's DROP frame, and the Task drop's wait-before-free
// joined each spawn at the loop-iteration brace — serializing spawn
// loops entirely (cajeta-http found it as Server.dispatch wedging its
// caller until the connection fiber ended). The fix hands the Task to
// the runtime scope frame (__cajeta_scope_register_owned): the frame
// joins at scope exit as always, and now also frees the scope-owned
// task — a per-site drop-entry alloca cannot represent N tasks alive at
// once from one loop.
//
// Test 1 deadlocks under the old lowering (iteration join waits on a
// token nobody has sent yet); ctest's timeout turns that into a failure.
// Test 2 pins that the join STILL happens at the scope brace (the fix
// must not turn scope joins into fire-and-forget).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& dBody) {
    std::string src =
        std::string("package test;\n")
        + "import cajeta.concurrent.Channel;\n"
        + "import cajeta.lang.Optional;\n"
        + "public final class D {\n" + dBody + "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// A loop of discarded spawns runs CONCURRENTLY: every sleeper must be
// launched (parked on `go`) before the tokens are sent — under the old
// iteration-brace join, iteration 0 wedged waiting for a token this
// function only sends after the loop.
TEST(SpawnScopeJoinTests, discardedSpawnLoopIsConcurrent) {
    EXPECT_EQ(runI32(
        "    public static async int32 sleeper(Channel<int32> go,\n"
        "                                      Channel<int32> done) {\n"
        "        Optional<int32> v = go.receive();\n"
        "        done.send(v.get());\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Channel<int32> go = heap Channel<int32>(4);\n"
        "        Channel<int32> done = heap Channel<int32>(4);\n"
        "        int32 i = 0;\n"
        "        while (i < 3) {\n"
        "            spawn sleeper(go, done);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        go.send(1);\n"
        "        go.send(2);\n"
        "        go.send(3);\n"
        "        int32 sum = 0;\n"
        "        i = 0;\n"
        "        while (i < 3) {\n"
        "            Optional<int32> d = done.receive();\n"
        "            sum = sum + d.get();\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return sum;\n"
        "    }\n"
    ), 6);
}

// The scope brace still JOINS: a child that writes its result only
// after receiving a token must have written it by the time control
// passes the `}` — the fix moves the join point, it must not remove it.
TEST(SpawnScopeJoinTests, explicitScopeStillJoinsAtBrace) {
    EXPECT_EQ(runI32(
        "    public static async int32 adder(Channel<int32> go, int32[] out) {\n"
        "        Optional<int32> v = go.receive();\n"
        "        out[0] = out[0] + v.get();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Channel<int32> go = heap Channel<int32>(2);\n"
        "        int32[] out = heap int32[1];\n"
        "        out[0] = 0;\n"
        "        scope {\n"
        "            spawn adder(go, out);\n"
        "            spawn adder(go, out);\n"
        "            go.send(20);\n"
        "            go.send(22);\n"
        "        }\n"
        "        return out[0];\n"                     // 42 iff the } joined
        "    }\n"
    ), 42);
}

// A throw that unwinds past a scope holding a DISCARDED (scope-owned)
// spawn must still join + free it — the fix moved task ownership from a
// drop entry onto the scope frame, and the throw path no longer walks
// the scope chain, so the join has to come from the catching function's
// scope_exit_to on the way out. The child writes its result only after
// receiving a token (sent before the throw); if it were not joined, the
// store could be lost or the freed task struct would corrupt.
TEST(SpawnScopeJoinTests, throwAcrossScopeOwnedSpawnStillJoins) {
    EXPECT_EQ(runI32(
        "    static AtomicInt32 joined = null;\n"
        "    public static async int32 child(Channel<int32> go) {\n"
        "        Optional<int32> v = go.receive();\n"
        "        D.joined.store(1);\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Channel<int32> go = heap Channel<int32>(1);\n"
        "        AtomicInt32 j = heap AtomicInt32(0);\n"
        "        D.joined = #j;\n"
        "        try {\n"
        "            scope {\n"
        "                spawn child(go);\n"
        "                go.send(1);\n"
        "                throw heap Exception(\"boom\");\n"
        "            }\n"
        "        } catch (Exception e) { }\n"
        "        return 1 - D.joined.load();\n"     // 0 iff the child was joined
        "    }\n"
    ), 0);
}
