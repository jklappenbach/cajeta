//
// S1 of the value-type operator-overloading mechanism
// (plans/value-type-overloading-plan.md): a class declared `@ValueType` is a
// by-value POD eligible for operator-overload dispatch. These tests cover the
// declaration side only — the visitor recognizes the annotation, runs the POD
// validator (no interface, no inherited fields, every field a primitive / Vector
// / another @ValueType), and ORs VALUE_TYPE_FLAG into the class. Dispatch (the
// relaxed gate) is S2.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "cajeta/type/CajetaType.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Flags of a compiled class found in the canonical map by name (the class is
// registered under both its short name and its qualified name); 0 if absent.
uint64_t flagsOf(const std::string& name) {
    auto& cmap = cajeta::CajetaType::getCanonicalMap();
    auto it = cmap.find(name);
    if (it != cmap.end() && it->second) return it->second->getTypeFlags();
    const std::string suffix = "." + name;
    for (auto& kv : cmap) {
        if (!kv.second) continue;
        const std::string& k = kv.first;
        if (k.size() >= suffix.size()
                && k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return kv.second->getTypeFlags();
        }
    }
    return 0;
}

} // namespace

// A valid @ValueType (primitive fields, no inheritance) compiles, runs, and
// carries VALUE_TYPE_FLAG.
TEST(ValueTypeFlagTests, validValueTypeCarriesFlag) {
    auto src =
        "package test;\n"
        "@ValueType public final class VtPoint {\n"
        "    public float32 x;\n"
        "    public float32 y;\n"
        "    public VtPoint(float32 x, float32 y) { this.x = x; this.y = y; }\n"
        "    public float32 total() { return this.x + this.y; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        VtPoint p = heap VtPoint(3.0f, 4.0f);\n"
        "        return (int32) p.total();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
    EXPECT_TRUE(flagsOf("VtPoint") & VALUE_TYPE_FLAG);
}

// A Vector field is accepted (Vector carries PRIMITIVE_FLAG, so it passes the
// POD field check) and the class still gains VALUE_TYPE_FLAG.
TEST(ValueTypeFlagTests, vectorFieldAccepted) {
    auto src =
        "package test;\n"
        "@ValueType public final class VtWithVec {\n"
        "    public Vector<float32, 4> v;\n"
        "    public int32 tag() { return 5; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    (void) runI32(src);
    EXPECT_TRUE(flagsOf("VtWithVec") & VALUE_TYPE_FLAG);
}

// A field whose type is a plain (non-value, non-primitive) class is rejected.
TEST(ValueTypeFlagTests, nonPodFieldRejected) {
    auto src =
        "package test;\n"
        "public class VtPlain { public int32 a; }\n"
        "@ValueType public final class VtBad {\n"
        "    public VtPlain p;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// @ValueType on an interface is rejected.
TEST(ValueTypeFlagTests, interfaceRejected) {
    auto src =
        "package test;\n"
        "@ValueType public interface VtIface {\n"
        "    public int32 op();\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// @ValueType with inherited fields is rejected (value types are flat POD).
TEST(ValueTypeFlagTests, inheritanceRejected) {
    auto src =
        "package test;\n"
        "public class VtBase { public int32 b; }\n"
        "@ValueType public final class VtSub extends VtBase {\n"
        "    public int32 s;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}
