//
// Interface behavioral tests. v1 scope:
//   - declare an interface with abstract method signatures
//   - a class implements an interface and provides matching methods
//   - dispatch through an interface-typed variable lands on the class's impl
//   - multiple classes implementing the same interface dispatch independently
//
// Not in v1 scope (separately tracked in GAP-1 follow-ups):
//   - default methods (interface methods with bodies)
//   - interface-extends-interface
//   - interface fields / constants
//   - compile-time check that a class actually implements every interface method
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

// Simplest case: interface + one implementing class + direct call on the
// concrete class. The class's own dispatch path should not be disturbed
// by the new interface machinery.
TEST(InterfaceTests, classWithInterfaceStillDispatchesDirectly) {
    auto src =
        "package test;\n"
        "public interface Greeter {\n"
        "    public int32 greet();\n"
        "}\n"
        "public class Hello implements Greeter {\n"
        "    public int32 greet() { return 42; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Hello h = heap Hello();\n"
        "        return h.greet();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// The crown jewel: dispatch through the *interface* type lands on the
// implementing class's method. The class's vtable carries an entry keyed
// by the interface method's hash, pointing to the class's concrete fn.
TEST(InterfaceTests, dispatchThroughInterfaceVariableLandsOnImpl) {
    auto src =
        "package test;\n"
        "public interface Greeter {\n"
        "    public int32 greet();\n"
        "}\n"
        "public class Hello implements Greeter {\n"
        "    public int32 greet() { return 7; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Greeter g = heap Hello();\n"
        "        return g.greet();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Two unrelated classes implement the same interface. A variable typed
// as the interface dispatches to each class's own implementation in turn.
TEST(InterfaceTests, twoImplementersDispatchIndependently) {
    auto src =
        "package test;\n"
        "public interface Greeter {\n"
        "    public int32 greet();\n"
        "}\n"
        "public class Aye implements Greeter {\n"
        "    public int32 greet() { return 3; }\n"
        "}\n"
        "public class Bee implements Greeter {\n"
        "    public int32 greet() { return 5; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Greeter a = heap Aye();\n"
        "        Greeter b = heap Bee();\n"
        "        return a.greet() + b.greet();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 8);
}
