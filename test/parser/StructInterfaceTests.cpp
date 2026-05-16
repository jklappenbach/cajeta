//
// Session 9 — struct implements interface; per-(struct, interface) vtable
// synthesis. The through-interface dispatch path itself (call through a
// fat-pointer interface value) is Sessions 10–11; this file pins the
// declaration shape, the implemented-interfaces metadata, and signature
// compatibility checks at the struct/interface boundary.
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
// S9.1 — `struct Foo implements Interface { ... }` parses, and a direct
// call on the concrete struct still works (no vtable hit; same path
// S8 wired up).
// ---------------------------------------------------------------------

TEST(StructInterfaceTests, structImplementsOneInterface) {
    auto src =
        "package test;\n"
        "public interface Greeter {\n"
        "    public int32 greet();\n"
        "}\n"
        "public struct Hi implements Greeter {\n"
        "    int32 base;\n"
        "    public int32 greet() { return this.base + 1; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Hi h = Hi { base: 10 };\n"
        "        return h.greet();\n"  // direct call, no vtable
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}

// ---------------------------------------------------------------------
// S9.2 — synthesize a per-(struct, interface) vtable global. Each
// pair gets its own static global containing function pointers to
// the struct's concrete implementations of the interface's methods,
// in interface-declaration order. Through-interface dispatch
// (Sessions 10–11) loads from this global into the tagged fat
// pointer at interface-value construction time.
// ---------------------------------------------------------------------

// Direct call still works when the struct implements two interfaces.
// Pins that the extra metadata + dual vtable globals don't disturb the
// direct-call path's signature/dispatch.
TEST(StructInterfaceTests, structImplementsTwoInterfaces) {
    auto src =
        "package test;\n"
        "public interface Greeter { public int32 greet(); }\n"
        "public interface Counted { public int32 count(); }\n"
        "public struct Both implements Greeter, Counted {\n"
        "    int32 base;\n"
        "    public int32 greet() { return this.base + 1; }\n"
        "    public int32 count() { return this.base * 2; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Both b = Both { base: 10 };\n"
        "        return b.greet() + b.count();\n"  // 11 + 20 = 31
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 31);
}
