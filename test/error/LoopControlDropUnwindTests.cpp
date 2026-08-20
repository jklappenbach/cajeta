//
// break/continue past a block-scoped owner must pop its drop-chain entry.
//
// Found by cajeta-jinja's Unit 4 statement tests (and minimized to ~80
// lines): `continue` (and `break`) branch straight to the latch/exit and
// skip the end-of-block __cajeta_drop_pop_run calls of every block opened
// inside the loop. The owner's stack-allocated drop entry then stays
// LINKED in the per-thread chain; the next iteration's declaration pushes
// the same slot again (self-link), or the frame dies with the entry still
// chained. Either way the next THROW walks the corrupted chain and
// crashes inside __cajeta_throw — far from the loop that caused it.
// Shipped in v0.21.0; fixed by recording the method's dropFrameStack
// depth in LoopContext and emitting pop_run down to that watermark at
// every break/continue site (the return path's emitOwnerDrops, bounded).
//
// The shape mirrors the minimized repro: recursion (block -> statement ->
// if -> block, like a template parser), an owner declared in a loop
// branch that exits via continue, and a throw a few frames later.
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
    return fn ? fn() : -1;
}

const char* kPrologue =
    "package test;\n"
    "import cajeta.error.RecoverableException;\n"
    "class BoomExc extends RecoverableException {\n"
    "    BoomExc(#String message) {\n"
    "        super(#message);\n"
    "        return;\n"
    "    }\n"
    "}\n";

TEST(LoopControlDropUnwindTests, ContinuePastOwnedLocalThenThrowUnwindsCleanly) {
    std::string src = kPrologue;
    src +=
        "class Machine {\n"
        "    String tag;\n"
        "    String keep;\n"
        "    Machine() { this.tag = null; this.keep = null; }\n"
        "    #String mint(int32 i) { return \"\" + \"value-\" + i; }\n"
        "    int32 sub(int32 depth) {\n"
        "        int32 i = 0;\n"
        "        while (i < 4) {\n"
        "            if (i == 0 && depth == 0) {\n"
        "                int32 r = this.stmt(depth);\n"
        "                i = i + 1;\n"
        "                continue;\n"
        "            }\n"
        "            if (i == 0) {\n"
        "                String a #= this.mint(100 + depth);\n"
        "                this.keep #= \"\" + a;\n"
        "                i = i + 1;\n"
        "                continue;\n"           // owner live at the jump
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "    int32 stmt(int32 depth) { return this.iff(depth); }\n"
        "    int32 iff(int32 depth) {\n"
        "        int32 r = this.sub(depth + 1);\n"
        "        this.boom(depth);\n"
        "        return r;\n"
        "    }\n"
        "    void boom(int32 line) {\n"
        "        throw heap BoomExc(\"mismatch at line \" + line);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Machine m = heap Machine();\n"
        "        try {\n"
        "            int32 r = m.sub(0);\n"
        "            return 0 - 3;\n"           // must not get here
        "        } catch (BoomExc e) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(1, runI32(src));
}

TEST(LoopControlDropUnwindTests, BreakPastOwnedLocalThenThrowUnwindsCleanly) {
    std::string src = kPrologue;
    src +=
        "public final class D {\n"
        "    static #String mint(int32 i) { return \"\" + \"v-\" + i; }\n"
        "    static int32 scan() {\n"
        "        int32 i = 0;\n"
        "        while (i < 5) {\n"
        "            if (i == 2) {\n"
        "                String b #= D.mint(i);\n"
        "                if (b.byteLength() > 0) {\n"
        "                    break;\n"          // two blocks deep
        "                }\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return i;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 s = D.scan();\n"
        "        try {\n"
        "            throw heap BoomExc(\"after break \" + s);\n"
        "        } catch (BoomExc e) {\n"
        "            return s;\n"               // 2 when clean
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(2, runI32(src));
}

// Every-iteration continue: N pushes of the SAME stack slot with no pop
// in between was the self-link path; with the fix each iteration pops at
// the continue site, so the chain stays balanced and the later throw
// unwinds through a clean chain.
TEST(LoopControlDropUnwindTests, EveryIterationContinueStaysBalanced) {
    std::string src = kPrologue;
    src +=
        "public final class D {\n"
        "    static #String mint(int32 i) { return \"\" + \"x-\" + i; }\n"
        "    public static int32 run() {\n"
        "        int32 total = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 16) {\n"
        "            String s #= D.mint(i);\n"
        "            total = total + s.byteLength();\n"
        "            i = i + 1;\n"
        "            continue;\n"
        "        }\n"
        "        try {\n"
        "            throw heap BoomExc(\"balance \" + total);\n"
        "        } catch (BoomExc e) {\n"
        "            if (total > 0) { return 1; }\n"
        "            return 0 - 2;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(1, runI32(src));
}

}  // namespace
