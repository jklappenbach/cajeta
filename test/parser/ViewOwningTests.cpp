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

TEST(ViewOwningTests, owningFormConstructsAndExecutes) {
    // The view takes the buffer; the original local is consumed. Use the
    // view normally; cleanup happens at scope exit.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view H {\n"
        "    int32 a;\n"
        "    int32 b;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[2];\n"
        "        H h = H(#bytes);\n"
        "        h.a = 13;\n"
        "        h.b = 29;\n"
        "        return h.a + h.b;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Helper for the next test — encode (borrow, owning) as a single int.
namespace {
constexpr int32_t encode(int32_t borrow, int32_t owning) {
    return (borrow << 8) | (owning & 0xff);
}
}

TEST(ViewOwningTests, owningFormBufferDroppedAtScopeExit) {
    // Drop count goes up by exactly 1 (the view's drop) at function exit.
    // Borrow form also sees 1 (the source array's own drop entry). The
    // count is read FROM THE CALLER, after the inner function has fully
    // returned and its scope has unwound — reading inside the inner
    // function would miss the drops since they fire on the way out.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view H {\n"
        "    int32 a;\n"
        "}\n"
        "public final class S {\n"
        "    public static void doOwning() {\n"
        "        int32[] bytes = heap int32[1];\n"
        "        H h = H(#bytes);\n"
        "        h.a = 1;\n"
        "    }\n"
        "    public static void doBorrow() {\n"
        "        int32[] bytes = heap int32[1];\n"
        "        H h = H(bytes);\n"
        "        h.a = 1;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        doOwning();\n"
        "        int64 owning = Cajeta.dropCount();\n"
        "        Cajeta.dropCountReset();\n"
        "        doBorrow();\n"
        "        int64 borrow = Cajeta.dropCount();\n"
        "        int32 oi = (int32) owning;\n"
        "        int32 bi = (int32) borrow;\n"
        "        return (bi << 8) | oi;\n"
        "    }\n"
        "}\n";
    int32_t got = runI32(src);
    EXPECT_EQ(got, encode(1, 1))
        << "borrow drops=" << ((got >> 8) & 0xff)
        << " owning drops=" << (got & 0xff);
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
        "        H h #= makeOwned();\n"
        "        return h.a;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}
