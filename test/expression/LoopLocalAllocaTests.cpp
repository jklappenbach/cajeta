//
// Loop-body local allocas must not grow the native stack (o0-loop-alloca plan
// Unit 1; spec §2). A local declared inside a loop body must reserve its slot
// ONCE (in the entry block), not re-alloca every iteration — an alloca in a loop
// re-allocates native stack each pass and overflows at -O0 (the JIT). These run
// loops with iteration counts that would blow an 8MB stack if the slot leaked.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <sstream>
#include <string>

using cajeta_test::CajetaJit;

// 1.1.1 — 2,000,000 iterations, one int64 body local. If the slot allocas per
// iteration that's 2M*8B = 16MB > 8MB stack -> overflow. Sum 0..N-1 checks value.
TEST(LoopLocalAllocaTests, ManyIterationsScalarBodyLocal) {
    const char* src =
        "package test;\n"
        "public final class A {\n"
        "    public static int64 run() {\n"
        "        int64 acc = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 2000000) {\n"
        "            int64 t = (int64) i;\n"   // loop-body local slot
        "            acc = acc + t;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return acc;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.A");
    auto fn = jit->lookup<int64_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    // sum_{0..1999999} = 1999999*2000000/2 = 1999999000000
    EXPECT_EQ(fn(), 1999999000000LL);
}

// 1.1.2 — several body locals (incl. recomputed each pass) over many iterations.
TEST(LoopLocalAllocaTests, MultipleBodyLocals) {
    const char* src =
        "package test;\n"
        "public final class A {\n"
        "    public static int64 run() {\n"
        "        int64 acc = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 1500000) {\n"
        "            int64 a = (int64) i;\n"
        "            int64 b = a + 1;\n"
        "            int64 c = a + b;\n"
        "            acc = acc + c;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return acc;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.A");
    auto fn = jit->lookup<int64_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    // c = a + b = i + (i+1) = 2i+1; sum_{0..N-1}(2i+1) = N^2, N=1500000
    EXPECT_EQ(fn(), 1500000LL * 1500000LL);
}

// 1.1.4 — value-semantics guard: the body local must read the CURRENT iteration's
// value (re-initialized each pass), not a stale carry-over. acc += t*t where
// t = i each pass; compare to sum of squares closed form.
TEST(LoopLocalAllocaTests, BodyLocalReinitializedEachIteration) {
    const char* src =
        "package test;\n"
        "public final class A {\n"
        "    public static int64 run() {\n"
        "        int64 acc = 0;\n"
        "        int64 i = 0;\n"
        "        while (i < 100000) {\n"
        "            int64 t = i;\n"
        "            acc = acc + t * t;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return acc;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.A");
    auto fn = jit->lookup<int64_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    // sum_{0..N-1} i^2 = (N-1)N(2N-1)/6, N=100000. The product (~2e15) fits in
    // int64 and is divisible by 6, so compute it exactly.
    int64_t N = 100000;
    int64_t expected = ((N - 1) * N * (2 * N - 1)) / 6;
    EXPECT_EQ(fn(), expected);
}

// 1.1.5 — IR check: the loop-body local's alloca lives in the function entry
// block, not the loop body. We assert the entry block (first block after the
// `define` line) contains an `alloca` and that allocas don't appear after a
// loop label. Simplest robust signal: count `alloca` — there are a small fixed
// number (locals), and they precede the first basic-block label inside run().
TEST(LoopLocalAllocaTests, SlotAllocaInEntryBlock) {
    const char* src =
        "package test;\n"
        "public final class A {\n"
        "    public static int64 run() {\n"
        "        int64 acc = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 10) {\n"
        "            int64 t = (int64) i;\n"
        "            acc = acc + t;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return acc;\n"
        "    }\n"
        "}\n";
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(src, "test.A", opts);
    auto fn = jit->lookup<int64_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 45); // sum 0..9

    // Extract run()'s body and confirm every `alloca` is in the ENTRY block —
    // i.e. precedes the first basic-block label. (A label line starts at
    // column 0 with an identifier ending in ':', e.g. `while_head:`.)
    std::string ir = jit->getModuleIr();
    size_t def = ir.find("define i64 @\"test.A::run()\"");
    ASSERT_NE(def, std::string::npos) << ir.substr(0, 1500);
    size_t bodyEnd = ir.find("\n}", def);
    std::string body = ir.substr(def, bodyEnd - def);

    std::istringstream iss(body);
    std::string line;
    size_t pos = 0, firstLabelPos = std::string::npos;
    bool seenDefine = false;
    while (std::getline(iss, line)) {
        if (!seenDefine) {
            if (line.find("define") != std::string::npos) seenDefine = true;
            pos += line.size() + 1;
            continue;
        }
        // A basic-block label starts at column 0 with an identifier + ':'.
        size_t colon = (!line.empty() && line[0] != ' ' && line[0] != '}')
            ? line.find(':') : std::string::npos;
        if (colon != std::string::npos) {
            std::string label = line.substr(0, colon);
            // Skip the entry block's own label — allocas live there. The first
            // NON-entry label (e.g. while_head) bounds the entry block.
            if (label != "entry") { firstLabelPos = pos; break; }
        }
        pos += line.size() + 1;
    }
    ASSERT_NE(firstLabelPos, std::string::npos) << "no successor label found:\n" << body;
    ASSERT_NE(body.find("alloca"), std::string::npos) << body;
    EXPECT_EQ(body.find("alloca", firstLabelPos), std::string::npos)
        << "an alloca appears in/after a loop block (not hoisted to entry):\n" << body;
}
