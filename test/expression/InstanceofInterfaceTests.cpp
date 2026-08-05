//
// InstanceofInterfaceTests — specs/instanceof-interface-lhs (defect,
// found 2026-08-04 in stdlib-completion U1): `instanceof` with an
// INTERFACE-typed lhs SIGSEGV'd — the fat body { data, vtable, kind }
// was handed to __cajeta_instanceof_named as if it were the object
// pointer. Acceptance 4.1-4.3:
//   4.1 the 9-line repro shape returns true;
//   4.2 interface-typed lhs answers truthfully for a matching class, a
//       non-matching class, and a SECOND INTERFACE (which also requires
//       implemented interfaces in the RTTI parent list);
//   4.3 the guarded form binds correctly from an interface-typed lhs.
// Plus: class-typed lhs against an implemented interface, and a
// null-valued interface local (false, no crash).
//

#include <gtest/gtest.h>

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

// Two interfaces; Circle implements both, Square implements Shape only.
const char* TYPES =
    "public interface Shape {\n"
    "    float64 area();\n"
    "}\n"
    "public interface Painted {\n"
    "    int32 hue();\n"
    "}\n"
    "public final class Circle implements Shape, Painted {\n"
    "    public float64 radius;\n"
    "    public Circle(float64 radius) {\n"
    "        this.radius = radius;\n"
    "    }\n"
    "    public float64 area() {\n"
    "        return 3.141592653589793 * this.radius * this.radius;\n"
    "    }\n"
    "    public int32 hue() {\n"
    "        return 42;\n"
    "    }\n"
    "}\n"
    "public final class Square implements Shape {\n"
    "    public float64 side;\n"
    "    public Square(float64 side) {\n"
    "        this.side = side;\n"
    "    }\n"
    "    public float64 area() {\n"
    "        return this.side * this.side;\n"
    "    }\n"
    "}\n";

} // namespace

// 4.1 + 4.2 (class targets) — the repro shape: interface-typed lhs,
// concrete-class target; true for the runtime type, false for another.
TEST(InstanceofInterfaceTests, interfaceLhsClassTarget) {
    std::string src = std::string("package test;\n") + TYPES +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Shape s = heap Circle(2.0);\n"
        "        if (s instanceof Circle) {\n"
        "        } else { return -1; }\n"
        "        if (s instanceof Square) { return -2; }\n"
        "        Shape q = heap Square(3.0);\n"
        "        if (q instanceof Square) {\n"
        "        } else { return -3; }\n"
        "        if (q instanceof Circle) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4.2 (interface target) — interface-typed lhs against a SECOND
// interface: true when the runtime class implements it, false otherwise.
TEST(InstanceofInterfaceTests, interfaceLhsInterfaceTarget) {
    std::string src = std::string("package test;\n") + TYPES +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Shape c = heap Circle(1.0);\n"
        "        if (c instanceof Painted) {\n"
        "        } else { return -1; }\n"
        "        Shape q = heap Square(1.0);\n"
        "        if (q instanceof Painted) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Class-typed lhs against an implemented interface — the RTTI parent
// list must carry implemented interfaces, not just superclasses.
TEST(InstanceofInterfaceTests, classLhsInterfaceTarget) {
    std::string src = std::string("package test;\n") + TYPES +
        "public final class D {\n"
        "    public static int32 check(Object o) {\n"
        "        int32 got = 0;\n"
        "        if (o instanceof Shape) { got = got + 1; }\n"
        "        if (o instanceof Painted) { got = got + 2; }\n"
        "        return got;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        if (D.check(heap Circle(1.0)) != 3) { return -1; }\n"
        "        if (D.check(heap Square(1.0)) != 1) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4.3 — the guarded form binds the OBJECT (not the fat body) from an
// interface-typed lhs: fields and methods of the bound name read
// correctly.
TEST(InstanceofInterfaceTests, guardedFormBindsFromInterfaceLhs) {
    std::string src = std::string("package test;\n") + TYPES +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Shape s = heap Circle(2.0);\n"
        "        if (s instanceof Circle c) {\n"
        "            if (c.radius != 2.0) { return -1; }\n"
        "            float64 a = c.area();\n"
        "            if (a < 12.56 || a > 12.57) { return -2; }\n"
        "        } else { return -3; }\n"
        "        if (s instanceof Square sq) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// A null-valued interface local: instanceof is FALSE, never a crash.
TEST(InstanceofInterfaceTests, nullInterfaceLhsIsFalse) {
    std::string src = std::string("package test;\n") + TYPES +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Shape s = null;\n"
        "        if (s instanceof Circle) { return -1; }\n"
        "        if (s instanceof Painted) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
