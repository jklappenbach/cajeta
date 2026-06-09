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
        "import cajeta.reflect.Field;\n"
        "import cajeta.reflect.Method;\n"
        "import cajeta.reflect.Constructor;\n"
        "import cajeta.reflect.Parameter;\n"
        "public class User {\n"
        "    public int32 id;\n"
        "    public int64 score;\n"
        "    public boolean active;\n"
        "    public User() { return; }\n"
        "    public User(int32 startId) { this.id = startId; return; }\n"
        "    public int32 bump() { this.id = this.id + 1; return this.id; }\n"
        "    public int32 addId(int32 delta) { return this.id + delta; }\n"
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

// REFL-3: data-driven typed field write/read roundtrip on int32 `id` (field 0).
TEST(ReflectionTests, fieldInt32Roundtrip) {
    EXPECT_EQ(runI32(
        "User u = heap User();\n"
        "Class c = Class.of(u);\n"
        "c.setInt32(u, 0, 41);\n"
        "return c.getInt32(u, 0);\n"), 41);
}

// REFL-3: int64 `score` (field 1) roundtrip (returned narrowed for the harness).
TEST(ReflectionTests, fieldInt64Roundtrip) {
    EXPECT_EQ(runI32(
        "User u = heap User();\n"
        "Class c = Class.of(u);\n"
        "c.setInt64(u, 1, (int64) 1000000);\n"
        "return (int32) c.getInt64(u, 1);\n"), 1000000);
}

// REFL-3: boolean `active` (field 2) roundtrip.
TEST(ReflectionTests, fieldBooleanRoundtrip) {
    EXPECT_EQ(runI32(
        "User u = heap User();\n"
        "Class c = Class.of(u);\n"
        "c.setBoolean(u, 2, true);\n"
        "return c.getBoolean(u, 2) ? 1 : 0;\n"), 1);
}

// REFL-3 × REFL-2B: a reflectively-set field is observed by a reflective
// invoke. Set id=41, then the no-arg bump() (returns id+1) must yield 42 —
// proving the field offset the setter writes matches what the method reads.
TEST(ReflectionTests, reflectiveSetVisibleToReflectiveInvoke) {
    EXPECT_EQ(runI32(
        "User u = heap User();\n"
        "Class c = Class.of(u);\n"
        "c.setInt32(u, 0, 41);\n"
        "int32 count = c.getMethodCount();\n"
        "int32 found = 0;\n"
        "int32 i = 0;\n"
        "while (i < count) {\n"
        "    if (c.getMethodParamCount(i) == 0) {\n"
        "        if (((int32) Class.invokeScalar0(u, i)) == 42) { found = 1; }\n"
        "    }\n"
        "    i = i + 1;\n"
        "}\n"
        "return found;\n"), 1);
}

// REFL-4 object model: Field object read/write roundtrip on `id` (field 0).
TEST(ReflectionTests, fieldObjectRoundtrip) {
    EXPECT_EQ(runI32(
        "User u = heap User();\n"
        "Field f = Class.of(u).getField(0);\n"
        "f.setInt32(u, 77);\n"
        "return f.getInt32(u);\n"), 77);
}

// REFL-4 object model: a Field object reports its declared name.
TEST(ReflectionTests, fieldObjectName) {
    EXPECT_EQ(runI32(
        "User u = heap User();\n"
        "Field f = Class.of(u).getField(0);\n"
        "return (f.getName() == \"id\") ? 1 : 0;\n"), 1);
}

// REFL-4 object model: invoke a no-arg method through a Method object. Scan
// Method objects for a zero-parameter one and invoke it; bump() yields 1.
TEST(ReflectionTests, methodObjectInvoke) {
    EXPECT_EQ(runI32(
        "User u = heap User();\n"
        "Class c = Class.of(u);\n"
        "int32 count = c.getMethodCount();\n"
        "int32 found = 0;\n"
        "int32 i = 0;\n"
        "while (i < count) {\n"
        "    Method m = c.getMethod(i);\n"
        "    if (m.getParameterCount() == 0) {\n"
        "        if (((int32) m.invokeScalar(u)) == 1) { found = 1; }\n"
        "    }\n"
        "    i = i + 1;\n"
        "}\n"
        "return found;\n"), 1);
}

// REFL-4 object model: "access down to the parameter" — find the 1-arg method
// (addId) via Method objects and read its parameter's name + type.
TEST(ReflectionTests, parameterIntrospection) {
    EXPECT_EQ(runI32(
        "User u = heap User();\n"
        "Class c = Class.of(u);\n"
        "int32 count = c.getMethodCount();\n"
        "int32 ok = 0;\n"
        "int32 i = 0;\n"
        "while (i < count) {\n"
        "    Method m = c.getMethod(i);\n"
        "    if (m.getParameterCount() == 1) {\n"
        "        Parameter p = m.getParameter(0);\n"
        "        if ((p.getName() == \"delta\") && (p.getTypeName() == \"int32\")) { ok = 1; }\n"
        "    }\n"
        "    i = i + 1;\n"
        "}\n"
        "return ok;\n"), 1);
}

// REFL-4 object model: construct via a Constructor object, verify validity.
TEST(ReflectionTests, constructorObjectNewInstance) {
    EXPECT_EQ(runI32(
        "User seed = heap User();\n"
        "Constructor ctor = Class.of(seed).getConstructor(0);\n"
        "Object o = ctor.heapInstance();\n"
        "return (o == null) ? -1 : Class.of(o).getFieldCount();\n"), 3);
}

// REFL-2C: User declares two constructors (User() and User(int32)).
TEST(ReflectionTests, constructorCountIsTwo) {
    EXPECT_EQ(runI32(
        "User u = heap User();\n"
        "return Class.of(u).getConstructorCount();\n"), 2);
}

// REFL-4 marshalling: invoke addId(delta) through a Method object with one
// argument. id=10, delta=5 -> 15. Proves the args buffer reaches the call.
TEST(ReflectionTests, methodInvokeWithArg) {
    EXPECT_EQ(runI32(
        "User u = heap User();\n"
        "Class c = Class.of(u);\n"
        "c.setInt32(u, 0, 10);\n"
        "int32 count = c.getMethodCount();\n"
        "int32 result = -1;\n"
        "int32 i = 0;\n"
        "while (i < count) {\n"
        "    Method m = c.getMethod(i);\n"
        "    if (m.getParameterCount() == 1) {\n"
        "        int64[] args = heap int64[1];\n"
        "        args[0] = (int64) 5;\n"
        "        result = (int32) m.invokeScalar(u, args);\n"
        "    }\n"
        "    i = i + 1;\n"
        "}\n"
        "return result;\n"), 15);
}

// REFL-4 marshalling: construct via the 1-arg User(int32 startId) through a
// Constructor object; the new instance's id is the passed argument (99).
TEST(ReflectionTests, constructorNewInstanceWithArg) {
    EXPECT_EQ(runI32(
        "User seed = heap User();\n"
        "Class c = Class.of(seed);\n"
        "int32 cc = c.getConstructorCount();\n"
        "int32 result = -1;\n"
        "int32 i = 0;\n"
        "while (i < cc) {\n"
        "    Constructor ctor = c.getConstructor(i);\n"
        "    if (ctor.getParameterCount() == 1) {\n"
        "        int64[] args = heap int64[1];\n"
        "        args[0] = (int64) 99;\n"
        "        Object o = ctor.heapInstance(args);\n"
        "        result = c.getInt32(o, 0);\n"
        "    }\n"
        "    i = i + 1;\n"
        "}\n"
        "return result;\n"), 99);
}

// REFL-2C: reflectively construct a User via the synthesized heapInstance
// adapter, then confirm the result is a valid, fully-formed instance — its
// vtable->classObject->rtti chain resolves and reports the right field count.
TEST(ReflectionTests, newInstanceProducesValidObject) {
    EXPECT_EQ(runI32(
        "User seed = heap User();\n"
        "Class c = Class.of(seed);\n"
        "Object o = c.heapInstance(0);\n"
        "return (o == null) ? -1 : Class.of(o).getFieldCount();\n"), 3);
}

// REFL-2C: a heapInstance'd object is functional — reflectively invoking the
// no-arg bump() on it returns 1 (its id was zero-initialized by construction).
TEST(ReflectionTests, newInstanceObjectIsFunctional) {
    EXPECT_EQ(runI32(
        "User seed = heap User();\n"
        "Class c = Class.of(seed);\n"
        "Object o = c.heapInstance(0);\n"
        "int32 count = c.getMethodCount();\n"
        "int32 found = 0;\n"
        "int32 i = 0;\n"
        "while (i < count) {\n"
        "    if (c.getMethodParamCount(i) == 0) {\n"
        "        if (((int32) Class.invokeScalar0(o, i)) == 1) { found = 1; }\n"
        "    }\n"
        "    i = i + 1;\n"
        "}\n"
        "return found;\n"), 1);
}
