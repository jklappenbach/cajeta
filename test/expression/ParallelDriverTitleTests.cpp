// stdlib-ownership-convention 8.4.2 — `ParallelDriver.reduceParallelChain`
// declares `#T` and never establishes a title on the empty path.
//
// Found while walking 8.1.1's inventory. The driver is declared
//
//     public static #T reduceParallelChain<T>(Stream<T> head, T seed, (T, T) -> #T fn)
//
// and BOTH of its returns are plain (`return accY;` ParallelDriver.cajeta:492,
// `return acc;` :541). A plain return under a `#` declaration takes the STATIC
// mode — flag 1, asserted unconditionally — and nothing challenges it: the
// TITLE_MISS guard covers `return #x` with a runtime flag, not a plain return
// under a `#` declaration.
//
// On an empty stream the accumulator loop never runs, so `accY` is still the
// caller's `seed`. The caller is then handed a TITLE over a value it only lent:
// its receiving local arms a drop, frees the lender's object, and the lender
// frees it again at its own scope exit.
//
// The sequential path is the control. `Stream.reduce` routes to `fold` when the
// stream is not parallel, and `fold` is mode-carrying (`return #acc`), so it
// hands back the borrow it was given. Same call, same seed, opposite outcome —
// which is what makes the parallel path's claim visibly wrong rather than
// merely unproven.
//
// One program, both cases: compiling `.parallel()` pulls in the whole fork/join
// driver at ~80s, and ParallelStreamP1Tests records that merging cases into one
// program is what keeps this suite affordable.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// run() returns parallelDelta * 10 + sequentialDelta, both measured as the
// change in live allocations across a scope that receives the reduce result.
//
//   0    both paths handed back a borrow — correct.
//   -10  the PARALLEL path handed back a title over the lent seed: the
//        receiving local's drop freed it (8.4.2).
//
// The seed is lent from a frame that keeps owning it, so the measurement is
// taken before the lender's own drop runs.
TEST(ParallelDriverTitleTests, emptyParallelReduceDoesNotForgeATitle) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "public class Cell {\n"
        "    public int32 n;\n"
        "    public Cell(int32 nn) { this.n = nn; }\n"
        "}\n"
        "public final class D {\n"
        "    // `seed` is PLAIN: the caller lends it and keeps the title.\n"
        "    static int32 viaParallel(Cell seed) {\n"
        "        Cell[] xs = heap Cell[1];\n"
        "        ArrayStream<Cell> s = heap ArrayStream<Cell>(xs, 0);\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        {\n"
        "            Cell out = s.parallel().reduce(seed, (a, b) -> a);\n"
        "            if (out.n != 3) { return -7; }\n"
        "        }\n"
        "        return (int32) (Cajeta.liveCount() - base);\n"
        "    }\n"
        "    static int32 viaSequential(Cell seed) {\n"
        "        Cell[] xs = heap Cell[1];\n"
        "        ArrayStream<Cell> s = heap ArrayStream<Cell>(xs, 0);\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        {\n"
        "            Cell out = s.reduce(seed, (a, b) -> a);\n"
        "            if (out.n != 3) { return -7; }\n"
        "        }\n"
        "        return (int32) (Cajeta.liveCount() - base);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 par = 0;\n"
        "        int32 seq = 0;\n"
        "        {\n"
        "            Cell owner = heap Cell(3);\n"
        "            par = viaParallel(owner);\n"
        "        }\n"
        "        {\n"
        "            Cell owner2 = heap Cell(3);\n"
        "            seq = viaSequential(owner2);\n"
        "        }\n"
        "        return par * 10 + seq;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}
