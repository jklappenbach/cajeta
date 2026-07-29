// Probe: moving a `#`-local of a class that has a user `drop()` into a field,
// then dropping the holder. If the `#`-move doesn't deactivate the source
// local's drop entry, the heap object is freed twice (double-free) -> crash.
// (TlsStream stores a #-moved TlsConnection, which has a user drop().)

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
int32_t runProbe(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}
} // namespace

// Class WITH a user drop(), #-moved into a field, holder dropped at scope exit.
TEST(MoveUserDropFieldProbe, moveUserDropClassIntoFieldThenDrop) {
    auto src =
        "package test;\n"
        "public class Res {\n"
        "    public int32 v;\n"
        "    public Res(int32 v) { this.v = v; }\n"
        "    public void drop() { }\n"   // user destructor
        "}\n"
        "public class Holder {\n"
        "    public Res r;\n"
        "    public Holder(#Res r) { this.r #= r; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Res x = heap Res(42);\n"
        "        Holder h = heap Holder(#x);\n"   // move x into h.r
        "        return h.r.v;\n"                  // 42; h drops here
        "    }\n"
        "}\n";
    EXPECT_EQ(runProbe(src), 42);
}
