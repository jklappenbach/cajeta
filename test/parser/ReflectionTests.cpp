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
        "    public float32 ratio;\n"
        "    public float64 precise;\n"
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

// Compile + run a complete module that defines its own classes and a
// `test.M.run() -> int32` entry. Used where the shared `User` fixture's
// method-scan tests would be perturbed (e.g. FP-return invoke needs a class
// with a single, index-stable FP-returning method).
int32_t runCustomI32(const std::string& fullSource) {
    auto jit = CajetaJit::compile(fullSource, "test.M");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// getClass() reaches the cached Class via the vtable's classObject slot.
TEST(ReflectionTests, getClassFieldCount) {
    EXPECT_EQ(runI32(
        "User u = heap User();\n"
        "Class c = Class.of(u);\n"
        "return c.getFieldCount();\n"), 5);
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

// REFL-3: float32 `ratio` (field 3) roundtrip. Written/read through the FP-typed
// accessor; checked by scaling to an int the harness can assert exactly.
TEST(ReflectionTests, fieldFloat32Roundtrip) {
    EXPECT_EQ(runI32(
        "User u = heap User();\n"
        "Class c = Class.of(u);\n"
        "c.setFloat32(u, 3, 2.5f);\n"
        "return (int32) (c.getFloat32(u, 3) * 4.0f);\n"), 10);
}

// REFL-3: float64 `precise` (field 4) roundtrip (returned scaled for the harness).
TEST(ReflectionTests, fieldFloat64Roundtrip) {
    EXPECT_EQ(runI32(
        "User u = heap User();\n"
        "Class c = Class.of(u);\n"
        "c.setFloat64(u, 4, 1.25);\n"
        "return (int32) (c.getFloat64(u, 4) * 8.0);\n"), 10);
}

// REFL-3 object model: Field object float32 roundtrip on `ratio` (field 3).
TEST(ReflectionTests, fieldObjectFloat32Roundtrip) {
    EXPECT_EQ(runI32(
        "User u = heap User();\n"
        "Field f = Class.of(u).getField(3);\n"
        "f.setFloat32(u, 3.5f);\n"
        "return (int32) (f.getFloat32(u) * 2.0f);\n"), 7);
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
        "return (o == null) ? -1 : Class.of(o).getFieldCount();\n"), 5);
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

// REFL-4.4 (Strategy 6): fiber-stack arg buffer, 1-arg form. Same addId(delta)
// call as methodInvokeWithArg, but the arg is passed directly (no heap int64[]);
// the native assembles the buffer on the fiber stack. id=10, delta=5 -> 15.
TEST(ReflectionTests, methodInvokeStackArg1) {
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
        "        result = m.invokeInt32(u, (int64) 5);\n"
        "    }\n"
        "    i = i + 1;\n"
        "}\n"
        "return result;\n"), 15);
}

// REFL-4.4: fiber-stack arg buffers, 2- and 3-arg forms. A custom class with
// sum2/sum3 (so the shared fixture's 1-arg method-scan tests are undisturbed);
// each arg is passed directly. base=100: sum2(7,9)=116, sum3(7,9,4)=120.
TEST(ReflectionTests, methodInvokeStackArgsMulti) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Method;\n"
        "public class Adder {\n"
        "    public int32 base;\n"
        "    public Adder() { return; }\n"
        "    public int32 sum2(int32 a, int32 b) { return this.base + a + b; }\n"
        "    public int32 sum3(int32 a, int32 b, int32 c) { return this.base + a + b + c; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Adder x = heap Adder();\n"
        "        Class c = Class.of(x);\n"
        "        c.setInt32(x, 0, 100);\n"
        "        int32 count = c.getMethodCount();\n"
        "        int32 r2 = -1;\n"
        "        int32 r3 = -1;\n"
        "        int32 i = 0;\n"
        "        while (i < count) {\n"
        "            Method m = c.getMethod(i);\n"
        "            if (m.getParameterCount() == 2) {\n"
        "                r2 = m.invokeInt32(x, (int64) 7, (int64) 9);\n"
        "            }\n"
        "            if (m.getParameterCount() == 3) {\n"
        "                r3 = m.invokeInt32(x, (int64) 7, (int64) 9, (int64) 4);\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return (r2 == 116) ? ((r3 == 120) ? 1 : 0) : 0;\n"
        "    }\n"
        "}\n"), 1);
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
        "return (o == null) ? -1 : Class.of(o).getFieldCount();\n"), 5);
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

// REFL-4.1 typed invoke: a float64-returning method comes back as a real FP
// value through Method.invokeFloat64 (the adapter stores the double in its FP
// register; the typed native reads it as a double, not int64-widened bits).
// Box has a single method (index 0) so the index is unambiguous.
TEST(ReflectionTests, invokeFloat64ReturnsRealValue) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Method;\n"
        "public class Box {\n"
        "    public float64 d;\n"
        "    public Box() { return; }\n"
        "    public float64 getD() { return this.d; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Box bx = heap Box();\n"
        "        Class c = Class.of(bx);\n"
        "        c.setFloat64(bx, 0, 3.25);\n"
        "        Method m = c.getMethod(0);\n"
        "        return (int32) (m.invokeFloat64(bx) * 4.0);\n"
        "    }\n"
        "}\n"), 13);
}

// REFL-4.1 typed invoke: float32 return via Method.invokeFloat32.
TEST(ReflectionTests, invokeFloat32ReturnsRealValue) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Method;\n"
        "public class Half {\n"
        "    public Half() { return; }\n"
        "    public float32 getHalf() { return 0.5f; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Half h = heap Half();\n"
        "        Method m = Class.of(h).getMethod(0);\n"
        "        return (int32) (m.invokeFloat32(h) * 6.0f);\n"
        "    }\n"
        "}\n"), 3);
}

// REFL-4.1 typed invoke: invokeInt32 narrows the int64 path for an
// int32-returning method.
TEST(ReflectionTests, invokeInt32Narrows) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Method;\n"
        "public class Seven {\n"
        "    public Seven() { return; }\n"
        "    public int32 get() { return 7; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Seven s = heap Seven();\n"
        "        Method m = Class.of(s).getMethod(0);\n"
        "        return m.invokeInt32(s);\n"
        "    }\n"
        "}\n"), 7);
}

// --- REFL-3.3: @Sealed visibility enforcement (decision D1) ------------------
// Reflection is DEFAULT-OPEN; a @Sealed class bars reflective access to its
// PRIVATE members only. The reflect API throws IllegalAccessException; the
// program catches it and returns 1 (a missing throw falls through to 0).

// A private field of a @Sealed class is blocked via the Field-object API.
TEST(ReflectionTests, sealedPrivateFieldThrows) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Field;\n"
        "import cajeta.reflect.IllegalAccessException;\n"
        "@Sealed\n"
        "public class Vault {\n"
        "    private int32 secret;\n"
        "    public int32 open;\n"
        "    public Vault() { this.secret = 42; this.open = 7; return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Vault v = heap Vault();\n"
        "        Field f = Class.of(v).getField(0);\n"   // secret (private)
        "        try {\n"
        "            int32 x = f.getInt32(v);\n"
        "            return 0;\n"
        "        } catch (IllegalAccessException e) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n"), 1);
}

// A PUBLIC field of a @Sealed class stays reachable (only private is barred).
TEST(ReflectionTests, sealedPublicFieldStillReadable) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Field;\n"
        "@Sealed\n"
        "public class Vault {\n"
        "    private int32 secret;\n"
        "    public int32 open;\n"
        "    public Vault() { this.secret = 42; this.open = 7; return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Vault v = heap Vault();\n"
        "        Field f = Class.of(v).getField(1);\n"   // open (public)
        "        return f.getInt32(v);\n"
        "    }\n"
        "}\n"), 7);
}

// The Class index-form accessor enforces the same gate.
TEST(ReflectionTests, sealedPrivateFieldIndexFormThrows) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.IllegalAccessException;\n"
        "@Sealed\n"
        "public class Vault {\n"
        "    private int32 secret;\n"
        "    public int32 open;\n"
        "    public Vault() { this.secret = 42; this.open = 7; return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Vault v = heap Vault();\n"
        "        Class c = Class.of(v);\n"
        "        try {\n"
        "            int32 x = c.getInt32(v, 0);\n"
        "            return 0;\n"
        "        } catch (IllegalAccessException e) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n"), 1);
}

// Default-open: a private field of a NON-sealed class is reflectively readable.
TEST(ReflectionTests, unsealedPrivateFieldReadable) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Field;\n"
        "public class Open {\n"
        "    private int32 secret;\n"
        "    public Open() { this.secret = 9; return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Open o = heap Open();\n"
        "        Field f = Class.of(o).getField(0);\n"   // secret (private, but not sealed)
        "        return f.getInt32(o);\n"
        "    }\n"
        "}\n"), 9);
}

// Invoking a private method of a @Sealed class throws.
TEST(ReflectionTests, sealedPrivateMethodInvokeThrows) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Method;\n"
        "import cajeta.reflect.IllegalAccessException;\n"
        "@Sealed\n"
        "public class Svc {\n"
        "    public Svc() { return; }\n"
        "    private int32 hidden() { return 5; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Svc s = heap Svc();\n"
        "        Method m = Class.of(s).getMethod(0);\n"   // hidden (private)
        "        try {\n"
        "            int32 x = m.invokeInt32(s);\n"
        "            return 0;\n"
        "        } catch (IllegalAccessException e) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n"), 1);
}

// Constructing through a private constructor of a @Sealed class throws.
TEST(ReflectionTests, sealedPrivateConstructorThrows) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.Object;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Constructor;\n"
        "import cajeta.reflect.IllegalAccessException;\n"
        "@Sealed\n"
        "public class Locked {\n"
        "    private Locked() { return; }\n"
        "    public Locked(int32 x) { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Locked seed = heap Locked(1);\n"
        "        Class c = Class.of(seed);\n"
        "        int32 count = c.getConstructorCount();\n"
        "        int32 i = 0;\n"
        "        int32 noArg = -1;\n"
        "        while (i < count) {\n"
        "            if (c.getConstructorParamCount(i) == 0) { noArg = i; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        Constructor ctor = c.getConstructor(noArg);\n"   // private no-arg
        "        try {\n"
        "            Object o = ctor.heapInstance();\n"
        "            return 0;\n"
        "        } catch (IllegalAccessException e) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n"), 1);
}

// --- REFL-4: reference-returning invoke (Method.invokeObject) ----------------
// A method that returns `heap Cell` is invoked reflectively; the returned
// #Object is owned (drop-tracked). We read its field reflectively (no downcast)
// to prove the real reference came back, not int64-widened bits.
TEST(ReflectionTests, invokeObjectReturnsReference) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.Object;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Method;\n"
        "public class Cell {\n"
        "    public int32 v;\n"
        "    public Cell(int32 x) { this.v = x; return; }\n"
        "}\n"
        "public class Factory {\n"
        "    public Factory() { return; }\n"
        "    public #Cell make() { return heap Cell(42); }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Factory f = heap Factory();\n"
        "        Method m = Class.of(f).getMethod(0);\n"   // make (only user method)
        "        Object o = m.invokeObject(f);\n"
        "        return (o == null) ? -1 : Class.of(o).getInt32(o, 0);\n"  // Cell.v
        "    }\n"
        "}\n"), 42);
}
