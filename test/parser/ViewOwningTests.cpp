//
// S3 — owning view variant (`View(#buf)`).
//
// The owning form transfers buffer ownership to the view: the view drops
// the underlying byte[] at scope exit via __cajeta_view_drop_owned. The
// borrow form (`View(buf)`) leaves ownership with the source.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace


// Helper for the next test — encode (borrow, owning) as a single int.
namespace {
constexpr int32_t encode(int32_t borrow, int32_t owning) {
    return (borrow << 8) | (owning & 0xff);
}
}


TEST(ViewOwningTests, borrowFormViewCannotEscape) {
    // Borrow form returning a view of a function-local buffer is
    // rejected with CAJETA_ERROR_VIEW_ESCAPE — the buffer would drop and
    // the caller would hold a dangling view.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view H {\n"
        "    int32 a;\n"
        "}\n"
        "public final class S {\n"
        "    public static H escape() {\n"
        "        int32[] bytes = heap int32[1];\n"
        "        H h = H(bytes);\n"
        "        return h;\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.S"));
}

TEST(ViewOwningTests, owningFormViewMayEscape) {
    // Owning form: the view owns the buffer, so returning it transfers
    // ownership to the caller. No escape error; the value is a real
    // owner that the caller's drop chain will reclaim.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view H {\n"
        "    int32 a;\n"
        "}\n"
        "public final class S {\n"
        "    public static #H makeOwned() {\n"
        "        int32[] bytes = heap int32[1];\n"
        "        H h = H(#bytes);\n"
        "        h.a = 7;\n"
        "        return h;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        H h = makeOwned();\n"
        "        return h.a;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}
