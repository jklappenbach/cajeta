//
// Phase B Unit 3 — drop-elision / move-opt PROBE.
// Measures drop-chain activity (Cajeta.dropCount()) in a fast primitive hot path
// (sort) vs an allocation-heavy path (string build). The contrast quantifies the
// CEILING a CIR drop-elision/move-opt pass could recover, feeding the go/no-go.
// NOT a feature — a measurement. Traces phase-b §5.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <string>
#include <cstdio>

using cajeta_test::CajetaJit;

namespace {

const char* kSrc =
    "package test;\n"
    "import cajeta.collection.Sort;\n"
    "public final class D {\n"
    // drops incurred sorting a 10-element primitive array (natural order).
    "    public static int64 sortDrops() {\n"
    "        int64[] a = { 9, 3, 7, 1, 5, 2, 8, 4, 6, 0 };\n"
    "        Cajeta.dropCountReset();\n"
    "        Sort.sort<int64>(a, 10);\n"
    "        return Cajeta.dropCount();\n"
    "    }\n"
    // drops incurred building a 100-char string by repeated concatenation.
    "    public static int64 stringDrops() {\n"
    "        Cajeta.dropCountReset();\n"
    "        String s = \"\";\n"
    "        int32 i = 0;\n"
    "        while (i < 100) {\n"
    "            s = s + \"x\";\n"
    "            i = i + 1;\n"
    "        }\n"
    "        return Cajeta.dropCount();\n"
    "    }\n"
    "}\n";

int64_t call(const char* fn) {
    auto jit = CajetaJit::compile(kSrc, "test.D");
    auto f = jit->lookup<int64_t (*)()>(fn);
    return f ? f() : -1;
}

} // namespace

TEST(DropProbeTests, dropCountsByWorkload) {
    int64_t sortD = call("sortDrops");
    int64_t strD = call("stringDrops");
    std::printf("[drop-probe] sort(10 int64): %lld drops | string-build(100 concat): %lld drops\n",
                (long long) sortD, (long long) strD);
    // The probe is the printed contrast; assert both ran (non-negative).
    EXPECT_GE(sortD, 0);
    EXPECT_GE(strD, 0);
}
