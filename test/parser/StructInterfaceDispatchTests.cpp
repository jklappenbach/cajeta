//
// Session 10 — struct/class → interface assignment + ownership.
//
// Covers the new functionality unlocked by the S9.5 fat-pointer
// foundation: assigning struct values to interface variables
// (BORROWED_STRUCT kind), the `#` operator for owned class→iface
// (OWNED_CLASS), the borrow tracker's struct-rooted iface escape
// rejection, and the kind-tag drop chain.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

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

// ---------------------------------------------------------------------
// S10.1 — struct → interface assignment.
//
// Assigning a struct value to an interface-typed local builds a fat
// pointer with kind = IFACE_KIND_BORROWED_STRUCT, data pointing at the
// struct's stack-resident body, and vtable pointing at the per-(struct,
// iface) global synthesized by S9.2. Dispatch through the interface
// value lands on the struct's concrete method.
// ---------------------------------------------------------------------

TEST(StructInterfaceDispatchTests, structToInterfaceDispatches) {
    auto src =
        "package test;\n"
        "public interface Greeter { public int32 greet(); }\n"
        "public struct Hi implements Greeter {\n"
        "    int32 base;\n"
        "    public int32 greet() { return this.base + 1; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Hi h = Hi { base: 10 };\n"
        "        Greeter g = h;\n"  // struct → interface
        "        return g.greet();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}
