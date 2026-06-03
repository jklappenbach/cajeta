//
// cajeta.threading.Tasks.withDeadline — absolute-instant sibling of
// withTimeout. Same Optional<R> reporting + cooperative cancellation;
// differs only in how the deadline is expressed (an absolute monotonic-
// clock nanos value rather than a Duration offset from now).
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

// Body finishes before the deadline expires; withDeadline returns
// Optional.present(value). Encodes present-ness as 1000+value so a
// single int32 return distinguishes empty (0), present-correct (1000+v),
// and present-wrong (would be off-by-one).
TEST(WithDeadlineTests, fastTaskReturnsPresentValue) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.threading.Tasks;\n"
        "public final class D {\n"
        "    public static async int32 compute() { return 7; }\n"
        "    public static int32 run() {\n"
        "        int64 deadline = Cajeta.currentTimeNanos() + 60000000000;\n"
        "        Task<int32> t = spawn compute();\n"
        "        Optional<int32> r = Tasks.withDeadline(deadline, t);\n"
        "        if (r.isPresent()) {\n"
        "            return 1000 + r.get();\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1007);
}

// Deadline that's already in the past at the call site → immediate
// timeout. Body's CPU loop still runs to completion (v1 cooperative
// cancellation doesn't preempt pure-CPU bodies), but Optional reports
// empty because the wait returned 0 immediately.
TEST(WithDeadlineTests, pastDeadlineReportsEmpty) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.threading.Tasks;\n"
        "public final class D {\n"
        "    public static async int32 slowCompute() {\n"
        "        int32 acc = 0;\n"
        "        for (int32 i = 0; i < 5000000; i = i + 1) {\n"
        "            acc = acc + 1;\n"
        "        }\n"
        "        return acc;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 deadline = Cajeta.currentTimeNanos() - 1;\n"
        "        Task<int32> t = spawn slowCompute();\n"
        "        Optional<int32> r = Tasks.withDeadline(deadline, t);\n"
        "        if (r.isPresent()) {\n"
        "            return 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
    "}\n";
    EXPECT_EQ(runI32(src), 0);
}
