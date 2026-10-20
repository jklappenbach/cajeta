// tensor-element-access Unit 5: in an --emit=exe build, a virtual call whose every
// implementation in the program is one function becomes a direct call. Each program is
// built with the pass and with CAJETA_DEVIRT=0; results must match and the IR must show
// the lookup gone only where nothing overrides.

#include <gtest/gtest.h>

#include <string>
#include "AotProgram.h"
#include "../jit/JitTestHelper.h"

using namespace cajeta_test_aot;

namespace {

const char* kBase =
    "package app;\n"
    "public class Base {\n"
    "    public Base() { }\n"
    "    public int32 f(int32 x) { return x + 1; }\n"
    "}\n";

const char* kDerivedNoOverride =
    "package app;\n"
    "public class Derived extends Base {\n"
    "    public Derived() { }\n"
    "    public int32 g() { return 7; }\n"
    "}\n";

const char* kDerivedOverride =
    "package app;\n"
    "public class Derived extends Base {\n"
    "    public Derived() { }\n"
    "    public int32 f(int32 x) { return x + 2; }\n"
    "}\n";

const char* kMain =
    "package app;\n"
    "public class Main {\n"
    "    static int32 callF(Base b, int32 x) { return b.f(x); }\n"
    "    public static int32 main(String[] args) {\n"
    "        Base a = heap Base();\n"
    "        Base d = heap Derived();\n"
    "        System.stdout.println(\"base \" + Main.callF(a, 41) + \" derived \" + Main.callF(d, 41));\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

/** True when callF still dispatches: it reads the receiver's vtable (the lookup itself may be inlined). */
bool callsLookup(const Built& b) {
    return bodiesOf(dumpedIr(b), "app.Main::callF").find("%vtable") != std::string::npos;
}

}  // namespace

// 5.1.1: nothing overrides Base.f, so callF calls it directly; CAJETA_DEVIRT=0 keeps the lookup.
TEST(DirectCall, aMethodNothingOverridesIsCalledDirectly) {
    Workspace w;
    w.write("src/app/Base.cajeta", kBase);
    w.write("src/app/Derived.cajeta", kDerivedNoOverride);
    w.write("src/app/Main.cajeta", kMain);
    Built on = compile(w, "on", "--emit=exe --opt=O2", "app.Main.main", "src");
    ASSERT_TRUE(on.ok) << on.log;
    Built off = compile(w, "off", "--emit=exe --opt=O2", "app.Main.main", "src", {{"CAJETA_DEVIRT", "0"}});
    ASSERT_TRUE(off.ok) << off.log;
    EXPECT_FALSE(callsLookup(on)) << bodiesOf(dumpedIr(on), "app.Main::callF");
    EXPECT_TRUE(callsLookup(off)) << "with CAJETA_DEVIRT=0 the lookup must remain";
    EXPECT_EQ(run(on).out, "base 42 derived 42\n");
    EXPECT_EQ(run(off).out, "base 42 derived 42\n");
}

// 5.1.2: an override keeps dispatch, and each receiver reaches its own body.
TEST(DirectCall, anOverriddenMethodKeepsDispatch) {
    Workspace w;
    w.write("src/app/Base.cajeta", kBase);
    w.write("src/app/Derived.cajeta", kDerivedOverride);
    w.write("src/app/Main.cajeta", kMain);
    Built on = compile(w, "on", "--emit=exe --opt=O2", "app.Main.main", "src");
    ASSERT_TRUE(on.ok) << on.log;
    EXPECT_TRUE(callsLookup(on)) << bodiesOf(dumpedIr(on), "app.Main::callF");
    EXPECT_EQ(run(on).out, "base 42 derived 43\n");
}

// 5.1.3: interface calls dispatch as before, through two implementations.
TEST(DirectCall, interfaceCallsDispatchAsBefore) {
    Workspace w;
    w.write("src/app/Shape.cajeta",
        "package app;\npublic interface Shape { int32 area(); }\n");
    w.write("src/app/Sq.cajeta",
        "package app;\npublic class Sq implements Shape {\n    public Sq() { }\n"
        "    public int32 area() { return 4; }\n}\n");
    w.write("src/app/Tri.cajeta",
        "package app;\npublic class Tri implements Shape {\n    public Tri() { }\n"
        "    public int32 area() { return 3; }\n}\n");
    w.write("src/app/Main.cajeta",
        "package app;\npublic class Main {\n"
        "    static int32 areaOf(Shape s) { return s.area(); }\n"
        "    public static int32 main(String[] args) {\n"
        "        Shape a = heap Sq();\n"
        "        Shape b = heap Tri();\n"
        "        System.stdout.println(\"sq \" + Main.areaOf(a) + \" tri \" + Main.areaOf(b));\n"
        "        return 0;\n"
        "    }\n}\n");
    Built on = compile(w, "on", "--emit=exe --opt=O2", "app.Main.main", "src");
    ASSERT_TRUE(on.ok) << on.log;
    EXPECT_EQ(run(on).out, "sq 4 tri 3\n");
}

// 5.1.4: Base and callF ship in one .cja and the override in a second; the exe keeps dispatch.
TEST(DirectCall, anOverrideInASecondArchiveKeepsDispatch) {
    Workspace w;
    w.write("lib1/lib1/Base.cajeta",
        "package lib1;\npublic class Base {\n    public Base() { }\n"
        "    public int32 f(int32 x) { return x + 1; }\n"
        "    public static int32 callF(Base b, int32 x) { return b.f(x); }\n}\n");
    w.write("lib2/lib2/Derived.cajeta",
        "package lib2;\nimport lib1.Base;\npublic class Derived extends Base {\n"
        "    public Derived() { }\n    public int32 f(int32 x) { return x + 2; }\n}\n");
    w.write("src/app/Main.cajeta",
        "package app;\nimport lib1.Base;\nimport lib2.Derived;\npublic class Main {\n"
        "    public static int32 main(String[] args) {\n"
        "        Base a = heap Base();\n"
        "        Base d = heap Derived();\n"
        "        System.stdout.println(\"base \" + Base.callF(a, 41) + \" derived \" + Base.callF(d, 41));\n"
        "        return 0;\n"
        "    }\n}\n");
    std::string lib1 = (w.root / "lib1.cja").string(), lib2 = (w.root / "lib2.cja").string();
    Built l1 = compile(w, "l1", "--emit=cja -o \"" + lib1 + "\"", "lib1.Base.callF", "lib1");
    ASSERT_TRUE(l1.ok) << l1.log;
    Built l2 = compile(w, "l2", "--emit=cja --classpath=\"" + lib1 + "\" -o \"" + lib2 + "\"",
                       "lib2.Derived.f", "lib2");
    ASSERT_TRUE(l2.ok) << l2.log;
    Built exe = compile(w, "exe", "--emit=exe --opt=O2 --classpath=\"" + lib1 + "," + lib2 + "\"",
                        "app.Main.main", "src");
    ASSERT_TRUE(exe.ok) << exe.log;
    EXPECT_EQ(run(exe).out, "base 42 derived 43\n");
}

// 5.1.5: a JIT build does not see the whole program, so it keeps the lookup.
TEST(DirectCall, aJitBuildKeepsTheLookup) {
    cajeta_test::CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = cajeta_test::CajetaJit::compile(
        "package test;\n"
        "public class B { public B() { } public int32 f(int32 x) { return x + 1; } }\n"
        "public final class D {\n"
        "    static int32 callF(B b) { return b.f(1); }\n"
        "    public static int32 run() { B b = heap B(); return D.callF(b); }\n"
        "}\n", "test.D", opts);
    std::string ir = jit->getModuleIr();
    EXPECT_NE(bodiesOf(ir, "test.D::callF").find("@__cajeta_vtable_lookup"), std::string::npos);
}

// A slot whose body tree-shaking removed keeps its lookup: a --release program on Tensor
// once linked against Tensor<boolean>::getAt, which survived only as a declaration.
TEST(DirectCall, aReleaseBuildKeepsLookupsForPrunedBodies) {
    Workspace w;
    w.write("src/app/Main.cajeta",
        "package app;\nimport cajeta.math.Tensor;\npublic class Main {\n"
        "    public static int32 main(String[] args) {\n"
        "        int64[] shape = heap int64[1];\n"
        "        shape[0] = 8;\n"
        "        Tensor<float64> t #= Tensor.zeros<float64>(shape);\n"
        "        int64 i = 0;\n"
        "        while (i < 8) { t.flatSet(i, (float64) i); i = i + 1; }\n"
        "        float64 s = 0.0;\n"
        "        i = 0;\n"
        "        while (i < 8) { s = s + t.flatGet(i); i = i + 1; }\n"
        "        System.stdout.println(\"sum \" + (int64) s);\n"
        "        return 0;\n"
        "    }\n}\n");
    Built b = compile(w, "rel", "--emit=exe --release", "app.Main.main", "src");
    ASSERT_TRUE(b.ok) << b.log;
    EXPECT_EQ(run(b).out, "sum 28\n");
}
