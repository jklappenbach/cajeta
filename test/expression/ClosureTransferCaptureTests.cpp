// A value moved into a closure with `#name` is owned by the closure and
// dropped with it, for class, String and closure captures alike.

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

std::string program(const std::string& body) {
    return
        "package test;\n"
        "import cajeta.lang.String;\n"
        "final class D { int32 v; D(int32 v) { this.v = v; } int32 apply(int32 x) { return this.v + x; } }\n"
        "final class Holder { (int32) -> int32 f; Holder() { this.f = null; } }\n"
        "public final class U {\n"
        "    static int32 body() {\n" + body +
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 last = U.body();\n"
        "        int64 live = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 100) { last = U.body(); i = i + 1; }\n"
        "        return Cajeta.liveCount() == live ? last : -1;\n"
        "    }\n"
        "}\n";
}

} // namespace

TEST(ClosureTransferCaptureTests, classCaptureDropsWithTheClosure) {
    EXPECT_EQ(runI32(program(
        "        D d = heap D(1);\n"
        "        (int32) -> int32 f = (int32 x) -> #d.apply(x);\n"
        "        return f(2) + f(3);\n")), 7);
}

TEST(ClosureTransferCaptureTests, classCaptureDropsWithAStoredClosure) {
    EXPECT_EQ(runI32(program(
        "        Holder h = heap Holder();\n"
        "        D d = heap D(4);\n"
        "        h.f #= (int32 x) -> #d.apply(x);\n"
        "        (int32) -> int32 g = h.f;\n"
        "        return g(1);\n")), 5);
}

TEST(ClosureTransferCaptureTests, stringCaptureDropsWithTheClosure) {
    EXPECT_EQ(runI32(program(
        "        String s = \"a string long enough to own a root buffer\" + 1;\n"
        "        (int32) -> int32 f = (int32 x) -> #s.byteLength() + x;\n"
        "        return f(0);\n")), 42);
}


TEST(ClosureTransferCaptureTests, lambdaStoredStraightIntoAFieldKeepsItsReturnType) {
    EXPECT_EQ(runI32(program(
        "        Holder h = heap Holder();\n"
        "        D d = heap D(4);\n"
        "        h.f = (int32 x) -> d.apply(x);\n"
        "        return h.f(1);\n")), 5);
}
