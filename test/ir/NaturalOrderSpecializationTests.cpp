//
// Phase B Unit 2 — natural-order operator specialization (VERIFY-first).
// The 2-arg natural-order Sort.sort(a, n) builds a non-capturing internal lambda
// and calls the 3-arg comparator sort. Phase A already specializes that — so the
// natural-order path devirtualizes with NO new Phase-B code. This test locks the
// finding in (and guards against regressing it). Traces phase-b §4.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// A natural-order sort — no comparator written by the caller.
const char* kSrc =
    "package test;\n"
    "import cajeta.collection.Sort;\n"
    "public final class D {\n"
    "    public static int32 run() {\n"
    "        int32[] a = [ 5, 3, 8, 1, 9, 2, 7 ];\n"
    "        Sort.sort<int32>(a, 7);\n"
    "        int32 i = 1;\n"
    "        while (i < 7) {\n"
    "            if (a[i] < a[i - 1]) { return 0; }\n"
    "            i = i + 1;\n"
    "        }\n"
    "        return a[0] * 100 + a[6];\n"   // sorted -> 1*100 + 9 = 109
    "    }\n"
    "}\n";

bool hasSpecialized(const std::string& ir, const std::string& methodName) {
    size_t pos = 0;
    while ((pos = ir.find("$spec$", pos)) != std::string::npos) {
        size_t from = pos > 200 ? pos - 200 : 0;
        if (ir.substr(from, pos - from).rfind(methodName) != std::string::npos)
            return true;
        pos += 6;
    }
    return false;
}

} // namespace

// 2.1.1 — a natural-order Sort.sort (no caller comparator) ALREADY devirtualizes
// via Phase A's internal-lambda specialization. The residual Phase-B gap for the
// common case is therefore empty.
TEST(NaturalOrderSpecializationTests, naturalOrderSortDevirtualizes) {
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(kSrc, "test.D", opts);
    std::string ir = jit->getModuleIr();
    ASSERT_FALSE(ir.empty());
    EXPECT_TRUE(hasSpecialized(ir, "sort"))
        << "natural-order Sort.sort should specialize (Phase A internal lambda)\n" << ir;
}

// 2.3.1 — and it sorts correctly.
