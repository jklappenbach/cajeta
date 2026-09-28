// Moving a class local into an interface-typed field builds the field's
// {data, vtable, kind} body from the class, and the field owns the value.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

std::string program(const std::string& install) {
    return
        "package test;\n"
        "import cajeta.lang.String;\n"
        "interface I { int32 f(int32 x); }\n"
        "final class N implements I {\n"
        "    int32 base;\n"
        "    N(int32 b) { this.base = b; }\n"
        "    public int32 f(int32 x) { return this.base + x; }\n"
        "}\n"
        "final class G<T> implements I {\n"
        "    int32 base;\n"
        "    G(int32 b) { this.base = b; }\n"
        "    public int32 f(int32 x) { return this.base * x; }\n"
        "}\n"
        "final class H {\n"
        "    I i;\n"
        "    H() { this.i = null; }\n"
        "}\n"
        "public final class U {\n"
        "    static void install(H h) {\n" + install +
        "    }\n"
        "    static int32 once() {\n"
        "        H h = heap H();\n"
        "        U.install(h);\n"
        "        return h.i.f(2);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 first = U.once();\n"
        "        int64 live = Cajeta.liveCount();\n"
        "        int32 again = U.once();\n"
        "        return Cajeta.liveCount() == live && first == again ? first : -1;\n"
        "    }\n"
        "}\n";
}

} // namespace

TEST(InterfaceFieldMoveStoreTests, sharpStoreOfAClassLocal) {
    EXPECT_EQ(runI32(program("        N g = heap N(40);\n        h.i #= g;\n")), 42);
}

TEST(InterfaceFieldMoveStoreTests, plainStoreOfAMovedLocal) {
    EXPECT_EQ(runI32(program("        N g = heap N(40);\n        h.i = #g;\n")), 42);
}

TEST(InterfaceFieldMoveStoreTests, sharpStoreOfAGenericClassLocal) {
    EXPECT_EQ(runI32(program("        G<String> g = heap G<String>(21);\n        h.i #= g;\n")), 42);
}

TEST(InterfaceFieldMoveStoreTests, directConstructionStillWorks) {
    EXPECT_EQ(runI32(program("        h.i = heap N(40);\n")), 42);
}
