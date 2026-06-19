//
// Virtual dispatch for value-returning methods (M5(a) in the value-return
// roadmap). A `stack`-constructed return travels through the sret + NRVO ABI
// (commit f9cebc3, docs/specification/lang/ValueReturns.md). The vtable slot's
// function-pointer type matches the sret signature on the concrete override,
// so a call through a base-typed receiver should land on the subclass's
// override and produce the override's value.
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

// Concrete receiver, value-returning method dispatched through the vtable
// (force-direct gate is gone — verifies the sret vtable slot type matches).
TEST(ValueReturnVirtualTests, concreteReceiverReturnsStackOptional) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public class Base {\n"
        "    public Optional<int32> pick() {\n"
        "        return stack Optional<int32>(true, 7);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Base b = heap Base();\n"
        "        Optional<int32> o = b.pick();\n"
        "        return o.get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Subclass override of a value-returning method. Receiver typed as Base,
// instance is Sub — dispatch must land on Sub::pick via the vtable, with the
// caller's sret slot threaded correctly across the override.
TEST(ValueReturnVirtualTests, overrideDispatchesThroughBaseReference) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public class Base {\n"
        "    public Optional<int32> pick() {\n"
        "        return stack Optional<int32>(true, 1);\n"
        "    }\n"
        "}\n"
        "public class Sub extends Base {\n"
        "    public Optional<int32> pick() {\n"
        "        return stack Optional<int32>(true, 99);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Base b = heap Sub();\n"
        "        Optional<int32> o = b.pick();\n"
        "        return o.get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}
