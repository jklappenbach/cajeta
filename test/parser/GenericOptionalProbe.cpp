//
// Probe: generic class with a method returning Optional<T> where T is
// the class's own type parameter. Needed for AbstractStream<T>.next()
// to land.
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

TEST(GenericOptionalProbe, genericClassWithTypedArrayField) {
    // Probe: does T[] work as a field on a generic class today?
    // Memory note says it's struct-only; this test confirms current
    // status before attempting ArrayStream<T>.
    auto src =
        "package test;\n"
        "public class Holder<T> {\n"
        "    T[] data;\n"
        "    public Holder(T[] d) { this.data = d; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] arr = {10, 20, 30};\n"
        "        Holder<int32> h = heap Holder<int32>(arr);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

TEST(GenericOptionalProbe, methodReturnsOptionalOfTypeParam) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public class Wrapper<T> {\n"
        "    T value;\n"
        "    public Wrapper(T v) { this.value = v; }\n"
        "    public Optional<T> getOpt() {\n"
        "        return heap Optional<T>(true, this.value);\n"
        "    }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Wrapper<int32> w = heap Wrapper<int32>(42);\n"
        "        Optional<int32> opt = w.getOpt();\n"
        "        if (opt.isPresent()) { return opt.get(); }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}
