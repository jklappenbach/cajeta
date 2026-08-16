//
// array-literals Unit 3: placement prefixes on array literals.
//   `[1,2,3]`        -> heap (default)
//   `stack [1,2,3]`  -> frame arena when the bound local proves non-escaping
//                       (primitive element), else heap
//   `shared [1,2,3]` -> device workgroup memory; host codegen rejects it
//
// Arena-vs-heap is observed with the Cajeta.arenaInUse()/liveCount() oracles,
// exactly as test/collections/ArenaArrayProbe.cpp does for creators: an arena
// allocation grows arena bytes and never touches the live-set; a heap one is
// the opposite.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
std::unique_ptr<CajetaJit> jitOf(const char* body) {
    std::string src =
        std::string("package test;\n") +
        "public final class A {\n" + body + "}\n";
    return CajetaJit::compile(src, "test.A");
}
} // namespace

// 3.1.1 — a bare `[1,2,3]` literal is heap by default (spec §4.1.1): it touches
// the live-set and does not grow the arena, even though it never escapes. (Bare
// literal placement is heap by design; only `stack` opts into the arena.)

// 3.1.2 — `stack [...]` parses and the values are correct.

// 3.1.3 — a non-escaping `stack [...]` primitive literal uses the arena, not the
// heap: arena bytes grow and the live-set is untouched.
TEST(ArrayLiteralPlacementTests, StackLiteralUsesArena) {
    auto jit = jitOf(
        "    public static int64 probe(int32 n) {\n"
        "        int64 abase = Cajeta.arenaInUse();\n"
        "        int64 lbase = Cajeta.liveCount();\n"
        "        int32[] a = stack [1, 2, 3];\n"              // stack -> arena
        "        a[0] = n;\n"
        "        int64 aused = Cajeta.arenaInUse() - abase;\n"
        "        int64 lused = Cajeta.liveCount() - lbase;\n"
        "        if (a[0] != n) { return -1; }\n"
        "        return aused * 1000 + lused;\n"              // aused>0, lused==0
        "    }\n");
    auto fn = jit->lookup<int64_t (*)(int32_t)>("probe");
    int64_t r = fn(7);
    EXPECT_GT(r / 1000, 0) << "stack literal did not use the arena";
    EXPECT_EQ(r % 1000, 0) << "stack literal touched the live-set";
}

// 3.1.3 (escape) — a `stack [...]` whose local escapes (returned) must fall back
// to heap rather than hand out a pointer into the reclaimed arena.

// 3.1.4 — `shared [...]` is device-only; the host codegen path rejects it.
