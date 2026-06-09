// REFL-1 — cajeta.reflect.Class foundation: getClass() (via the static
// Class.of factory), read-only introspection over the fixed-layout RTTI, and
// the vtable->classObject->rtti runtime chain. Each test compiles a small
// program that reflects over a fixture class and returns an int32 the C++
// side asserts on (String results are checked in-cajeta via `==` against a
// literal, returning 1/0).

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Fixture class + an M.run() entry that the body fills in.
std::string prog(const std::string& body) {
    return
        "package test;\n"
        "import cajeta.reflect.Class;\n"
        "public class User {\n"
        "    public int32 id;\n"
        "    public int64 score;\n"
        "    public boolean active;\n"
        "    public User() { return; }\n"
        "    public int32 bump() { this.id = this.id + 1; return this.id; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
}

int32_t runI32(const std::string& body) {
    auto jit = CajetaJit::compile(prog(body), "test.M");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// getClass() reaches the cached Class via the vtable's classObject slot.
TEST(ReflectionTests, getClassFieldCount) {
    EXPECT_EQ(runI32(
        "User u = heap User();\n"
        "Class c = Class.of(u);\n"
        "return c.getFieldCount();\n"), 3);
}

// The class declares at least bump(); method count is non-zero.
TEST(ReflectionTests, getClassMethodCountNonZero) {
    EXPECT_GE(runI32(
        "User u = heap User();\n"
        "return Class.of(u).getMethodCount();\n"), 1);
}

// Canonical name read from the RTTI typeName pointer.
TEST(ReflectionTests, getNameMatchesCanonical) {
    EXPECT_EQ(runI32(
        "User u = heap User();\n"
        "String n = Class.of(u).getName();\n"
        "return (n == \"test.User\") ? 1 : 0;\n"), 1);
}

// Class modifier flags carry PUBLIC (0x02).
TEST(ReflectionTests, classIsPublic) {
    EXPECT_EQ(runI32(
        "User u = heap User();\n"
        "return Class.of(u).isPublic() ? 1 : 0;\n"), 1);
}

// First declared field name comes back from the field table.
TEST(ReflectionTests, firstFieldNameIsId) {
    EXPECT_EQ(runI32(
        "User u = heap User();\n"
        "String f = Class.of(u).getFieldName(0);\n"
        "return (f == \"id\") ? 1 : 0;\n"), 1);
}

// Instance size is the RTTI allocationSize (non-zero for a real class).
TEST(ReflectionTests, instanceSizeNonZero) {
    EXPECT_GT(runI32(
        "User u = heap User();\n"
        "return (int32) Class.of(u).getInstanceSize();\n"), 0);
}

// REFL-2A: data-driven field offsets. User = { i32 id, i64 score, boolean
// active } behind an 8-byte vtable header. Offsets must be past the header,
// declaration-monotonic, the i64 field 8-aligned, and all within the instance
// size. (Exact offsets aren't asserted to stay robust to layout choices.)
TEST(ReflectionTests, fieldOffsetsAreSaneAndAligned) {
    EXPECT_EQ(runI32(
        "User u = heap User();\n"
        "Class c = Class.of(u);\n"
        "int32 o0 = c.getFieldOffset(0);\n"   // id    (int32)
        "int32 o1 = c.getFieldOffset(1);\n"   // score (int64)
        "int32 o2 = c.getFieldOffset(2);\n"   // active (boolean)
        "boolean ok = (o0 >= 8) && (o1 > o0) && (o2 > o1)\n"
        "    && ((o1 % 8) == 0)\n"
        "    && (((int64) o2) < c.getInstanceSize());\n"
        "return ok ? 1 : 0;\n"), 1);
}

// The field type-flag word is non-zero and carries the primitive bit (0x1)
// for a primitive field (id : int32).
TEST(ReflectionTests, fieldTypeFlagsCarryPrimitiveBit) {
    EXPECT_EQ(runI32(
        "User u = heap User();\n"
        "int64 f = Class.of(u).getFieldTypeFlags(0);\n"
        "return ((f & 1) != 0) ? 1 : 0;\n"), 1);
}

// REFL-2B: the synthesized per-class invoke adapter dispatches a reflective
// no-arg method call to a direct LLVM call. Scan User's no-arg methods and
// invoke each on the instance; bump() (id starts 0, returns id+1 = 1) must be
// reached through the adapter. Only zero-parameter methods are invoked (the
// no-arg entry passes a null arg buffer), so this stays crash-safe.
TEST(ReflectionTests, invokeReachesNoArgMethod) {
    EXPECT_EQ(runI32(
        "User u = heap User();\n"
        "Class c = Class.of(u);\n"
        "int32 count = c.getMethodCount();\n"
        "int32 found = 0;\n"
        "int32 i = 0;\n"
        "while (i < count) {\n"
        "    if (c.getMethodParamCount(i) == 0) {\n"
        "        int64 r = Class.invokeScalar0(u, i);\n"
        "        if (((int32) r) == 1) { found = 1; }\n"
        "    }\n"
        "    i = i + 1;\n"
        "}\n"
        "return found;\n"), 1);
}
