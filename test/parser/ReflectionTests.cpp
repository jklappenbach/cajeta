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

// The class declares at least bump(); method count is non-zero.

// Canonical name read from the RTTI typeName pointer.

// Class modifier flags carry PUBLIC (0x02).

// First declared field name comes back from the field table.

// Instance size is the RTTI allocationSize (non-zero for a real class).

// REFL-2A: data-driven field offsets. User = { i32 id, i64 score, boolean
// active } behind an 8-byte vtable header. Offsets must be past the header,
// declaration-monotonic, the i64 field 8-aligned, and all within the instance
// size. (Exact offsets aren't asserted to stay robust to layout choices.)

// The field type-flag word is non-zero and carries the primitive bit (0x1)
// for a primitive field (id : int32).

// REFL-2B: the synthesized per-class invoke adapter dispatches a reflective
// no-arg method call to a direct LLVM call. Scan User's no-arg methods and
// invoke each on the instance; bump() (id starts 0, returns id+1 = 1) must be
// reached through the adapter. Only zero-parameter methods are invoked (the
// no-arg entry passes a null arg buffer), so this stays crash-safe.

// REFL-3: data-driven typed field write/read roundtrip on int32 `id` (field 0).

// REFL-3: int64 `score` (field 1) roundtrip (returned narrowed for the harness).

// REFL-3: boolean `active` (field 2) roundtrip.

// REFL-3: float32 `ratio` (field 3) roundtrip. Written/read through the FP-typed
// accessor; checked by scaling to an int the harness can assert exactly.

// REFL-3: float64 `precise` (field 4) roundtrip (returned scaled for the harness).

// REFL-3 object model: Field object float32 roundtrip on `ratio` (field 3).

// REFL-3 × REFL-2B: a reflectively-set field is observed by a reflective
// invoke. Set id=41, then the no-arg bump() (returns id+1) must yield 42 —
// proving the field offset the setter writes matches what the method reads.

// REFL-4 object model: Field object read/write roundtrip on `id` (field 0).

// REFL-4 object model: a Field object reports its declared name.

// REFL-4 object model: invoke a no-arg method through a Method object. Scan
// Method objects for a zero-parameter one and invoke it; bump() yields 1.

// REFL-4 object model: "access down to the parameter" — find the 1-arg method
// (addId) via Method objects and read its parameter's name + type.

// REFL-4 object model: construct via a Constructor object, verify validity.

// REFL-2C: User declares two constructors (User() and User(int32)).

// REFL-4 marshalling: invoke addId(delta) through a Method object with one
// argument. id=10, delta=5 -> 15. Proves the args buffer reaches the call.

// REFL-4.4 (Strategy 6): fiber-stack arg buffer, 1-arg form. Same addId(delta)
// call as methodInvokeWithArg, but the arg is passed directly (no heap int64[]);
// the native assembles the buffer on the fiber stack. id=10, delta=5 -> 15.

// REFL-4.4: fiber-stack arg buffers, 2- and 3-arg forms. A custom class with
// sum2/sum3 (so the shared fixture's 1-arg method-scan tests are undisturbed);
// each arg is passed directly. base=100: sum2(7,9)=116, sum3(7,9,4)=120.

// REFL-4 marshalling: construct via the 1-arg User(int32 startId) through a
// Constructor object; the new instance's id is the passed argument (99).

// REFL-2C: reflectively construct a User via the synthesized heapInstance
// adapter, then confirm the result is a valid, fully-formed instance — its
// vtable->classObject->rtti chain resolves and reports the right field count.

// REFL-2C: a heapInstance'd object is functional — reflectively invoking the
// no-arg bump() on it returns 1 (its id was zero-initialized by construction).

// REFL-4.1 typed invoke: a float64-returning method comes back as a real FP
// value through Method.invokeFloat64 (the adapter stores the double in its FP
// register; the typed native reads it as a double, not int64-widened bits).
// Box has a single method (index 0) so the index is unambiguous.

// REFL-4.1 typed invoke: float32 return via Method.invokeFloat32.

// REFL-4.1 typed invoke: invokeInt32 narrows the int64 path for an
// int32-returning method.

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
        "        Class<?> c = Class.of(v);\n"
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
        "        Class<?> c = Class.of(seed);\n"
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

// REFL-4.1 boxing (W5): invokeBoxed hands back the right cajeta.lang wrapper for
// each primitive return, read back through the wrapper's field-0 value via the
// matching typed accessor. base=100: asI(5)->Int32(105), asD->Float64(2.5),
// asB->Boolean(true). Three distinct param counts keep the scan off the
// inherited 0-param methods (hash/toString/clone). ok reaches 3.

// REFL-4.1 boxing: a reference return passes through invokeObject (the boxed
// #Object IS the returned instance), and a void method boxes to null while still
// running its side effect.

// REFL-4.1 boxing: a primitive with no wrapper (int128 — doesn't fit the 64-bit
// boxing paths) raises UnsupportedReflectionException rather than widening or
// returning null. The method takes 4 params so the scan ignores the inherited
// 0-param methods. (int8/int16 etc. are now boxable as of W2.)

// REFL-4.1 boxing (W5b): Field.getBoxed / Class.getBoxed read each primitive
// field as its cajeta.lang wrapper, verified through the wrapper's field-0 value
// via the matching typed accessor. All five W1 field types round-trip; ok == 5.

// REFL-4.1 boxing (W5b): a reference field is ownership-unsafe to box (handing
// the held reference back as an owned #Object would double-drop), so getBoxed
// raises UnsupportedReflectionException rather than returning it.

// W2 boxing: invokeBoxed yields the right wrapper for narrow/unsigned/char
// returns. Type checked via Class.of(result).getName(); value via a Number
// downcast (asInt64) / a Char downcast. i16->-300, u32->200000, ch->'Z'. ok==3.

// W2 boxing: getBoxed reads each narrow/unsigned/char field as the right wrapper
// (width-correct loads). Type via getName, value via a Number/Char downcast.
// All seven W2 field types round-trip; ok == 7.

// --- REFL-6a: annotation NAME reflection -------------------------------------
// Annotation names ride the RTTI for every owner (class / field / method /
// constructor / parameter). A bare `@Foo` serializes to the canonical
// `"code.Foo"` (single-identifier annotation names default to the `code`
// package). Argument values are REFL-6b (not emitted yet). The fixture below
// uses arbitrary user annotations — cajeta records any annotation identifier;
// none need to be predeclared.

// A class annotation is enumerable and reports its canonical name.
TEST(ReflectionTests, classAnnotationName) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "@Service\n"
        "public class Widget {\n"
        "    public int32 id;\n"
        "    public Widget() { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Widget w = heap Widget();\n"
        "        Class<?> c = Class.of(w);\n"
        "        if (c.getAnnotationCount() != 1) { return 10; }\n"
        "        if (!c.getAnnotationName(0).equals(\"code.Service\")) { return 11; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"), 0);
}

// hasAnnotation matches the exact canonical name; a missing one is false.
TEST(ReflectionTests, classHasAnnotation) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "@Service\n"
        "public class Widget {\n"
        "    public Widget() { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Class<?> c = Class.of(heap Widget());\n"
        "        if (!c.hasAnnotation(\"code.Service\")) { return 1; }\n"
        "        if (c.hasAnnotation(\"code.Nope\")) { return 2; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"), 0);
}

// An Annotation object reports its name (and toString is the same).
TEST(ReflectionTests, annotationObjectName) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Annotation;\n"
        "@Service\n"
        "public class Widget {\n"
        "    public Widget() { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Annotation a = Class.of(heap Widget()).getAnnotation(0);\n"
        "        if (!a.getName().equals(\"code.Service\")) { return 1; }\n"
        "        if (!a.toString().equals(\"code.Service\")) { return 2; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"), 0);
}

// A plain class with no annotations reports a zero count.
TEST(ReflectionTests, classNoAnnotationsZeroCount) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.reflect.Class;\n"
        "public class Bare {\n"
        "    public Bare() { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        return Class.of(heap Bare()).getAnnotationCount();\n"
        "    }\n"
        "}\n"), 0);
}

// A field annotation is readable; an unannotated field reports zero.
TEST(ReflectionTests, fieldAnnotationName) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Field;\n"
        "public class Widget {\n"
        "    @Wired public int32 id;\n"
        "    public int32 plain;\n"
        "    public Widget() { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Class<?> c = Class.of(heap Widget());\n"
        "        Field id = c.getField(0);\n"
        "        if (id.getAnnotationCount() != 1) { return 10; }\n"
        "        if (!id.getAnnotationName(0).equals(\"code.Wired\")) { return 11; }\n"
        "        if (!id.hasAnnotation(\"code.Wired\")) { return 12; }\n"
        "        if (c.getField(1).getAnnotationCount() != 0) { return 13; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"), 0);
}

// A METHOD annotation is readable — this exercises the REFL-6a emission gap that
// was filled (method annotations weren't in the RTTI before). Found by scanning
// for the single annotated method, so the inherited-method indices don't matter.
TEST(ReflectionTests, methodAnnotationName) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Method;\n"
        "public class Widget {\n"
        "    public Widget() { return; }\n"
        "    @Audited public int32 ping() { return 1; }\n"
        "    public int32 quiet() { return 2; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Class<?> c = Class.of(heap Widget());\n"
        "        int32 ok = 0;\n"
        "        int32 i = 0;\n"
        "        int32 n = c.getMethodCount();\n"
        "        while (i < n) {\n"
        "            Method m = c.getMethod(i);\n"
        "            if (m.getAnnotationCount() > 0) {\n"
        "                if (m.getAnnotationName(0).equals(\"code.Audited\")) { ok = ok + 1; }\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return ok;\n"   // exactly one annotated method
        "    }\n"
        "}\n"), 1);
}

// A CONSTRUCTOR annotation is readable (same shared #MethodDesc shape).
TEST(ReflectionTests, constructorAnnotationName) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Constructor;\n"
        "public class Widget {\n"
        "    public Widget() { return; }\n"
        "    @Inject public Widget(int32 x) { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Class<?> c = Class.of(heap Widget());\n"
        "        int32 ok = 0;\n"
        "        int32 i = 0;\n"
        "        int32 n = c.getConstructorCount();\n"
        "        while (i < n) {\n"
        "            Constructor ct = c.getConstructor(i);\n"
        "            if (ct.getAnnotationCount() > 0) {\n"
        "                if (ct.getAnnotationName(0).equals(\"code.Inject\")) { ok = ok + 1; }\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return ok;\n"   // exactly one annotated constructor
        "    }\n"
        "}\n"), 1);
}

// A PARAMETER annotation is readable down at the parameter level.
TEST(ReflectionTests, parameterAnnotationName) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Method;\n"
        "import cajeta.reflect.Parameter;\n"
        "public class Widget {\n"
        "    public Widget() { return; }\n"
        "    public int32 scaled(@Bound int32 factor) { return factor; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Class<?> c = Class.of(heap Widget());\n"
        "        int32 ok = 0;\n"
        "        int32 i = 0;\n"
        "        int32 n = c.getMethodCount();\n"
        "        while (i < n) {\n"
        "            Method m = c.getMethod(i);\n"
        "            if (m.getParameterCount() == 1) {\n"
        "                Parameter p = m.getParameter(0);\n"
        "                if (p.getAnnotationCount() == 1) {\n"
        "                    if (p.getAnnotationName(0).equals(\"code.Bound\")) { ok = ok + 1; }\n"
        "                }\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return ok;\n"   // exactly one 1-arg method, its param annotated
        "    }\n"
        "}\n"), 1);
}

// Multiple annotations on one class are all enumerated, in declared order.
TEST(ReflectionTests, multipleClassAnnotations) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "@Service @Audited\n"
        "public class Widget {\n"
        "    public Widget() { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Class<?> c = Class.of(heap Widget());\n"
        "        if (c.getAnnotationCount() != 2) { return 10; }\n"
        "        if (!c.getAnnotationName(0).equals(\"code.Service\")) { return 11; }\n"
        "        if (!c.getAnnotationName(1).equals(\"code.Audited\")) { return 12; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"), 0);
}

// --- REFL-6b: annotation ARGUMENT values -------------------------------------
// Annotation arguments (`@Order(2)`, `@Component(name="disk")`, `@Profile(
// "prod")`, `@Cacheable(true)`, `@Encoding(Foo.class)`) now ride the RTTI as
// #AnnotationArgDesc rows. The Annotation object reads them by index or, with
// kind-checking, by key. The single-unnamed-arg form is read with key "value".

// Unnamed integer argument: @Order(2) -> getInt("value") == 2.
TEST(ReflectionTests, classAnnotationIntArg) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Annotation;\n"
        "@Order(2)\n"
        "public class Widget {\n"
        "    public Widget() { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Annotation a = Class.of(heap Widget()).getAnnotation(0);\n"
        "        if (a.getArgCount() != 1) { return 10; }\n"
        "        if (a.getInt(\"value\") != (int64) 2) { return 11; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"), 0);
}

// Named string argument: @Component(name = "disk") -> getString("name").
TEST(ReflectionTests, classAnnotationNamedStringArg) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Annotation;\n"
        "@Component(name = \"disk\")\n"
        "public class Widget {\n"
        "    public Widget() { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Annotation a = Class.of(heap Widget()).getAnnotation(0);\n"
        "        if (a.getArgCount() != 1) { return 10; }\n"
        "        if (!a.getArgName(0).equals(\"name\")) { return 11; }\n"
        "        if (!a.getString(\"name\").equals(\"disk\")) { return 12; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"), 0);
}

// Unnamed string argument read with the "value" key: @Profile("prod").
TEST(ReflectionTests, classAnnotationUnnamedStringArg) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Annotation;\n"
        "@Profile(\"prod\")\n"
        "public class Widget {\n"
        "    public Widget() { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Annotation a = Class.of(heap Widget()).getAnnotation(0);\n"
        "        if (!a.getString(\"value\").equals(\"prod\")) { return 11; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"), 0);
}

// Boolean argument: @Cacheable(true) -> getBool("value") == true.
TEST(ReflectionTests, classAnnotationBoolArg) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Annotation;\n"
        "@Cacheable(true)\n"
        "public class Widget {\n"
        "    public Widget() { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Annotation a = Class.of(heap Widget()).getAnnotation(0);\n"
        "        if (!a.getBool(\"value\")) { return 11; }\n"
        "        if (a.getArgKind(0) != 2) { return 12; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"), 0);
}

// Class-literal argument: @Refers(Marker.class) -> getClassRef -> "Marker"
// (classifyLiteral strips the `.class` suffix). `Refers` is a neutral
// annotation (no compiler behavior); `Marker` is a separate declared
// annotation type — a self-reference (`Widget.class` on Widget) would re-enter
// Widget's reflect class-object build and an active annotation like @Encoding
// would invoke its own subsystem.
TEST(ReflectionTests, classAnnotationClassRefArg) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Annotation;\n"
        "annotation Marker { }\n"
        "@Refers(Marker.class)\n"
        "public class Widget {\n"
        "    public Widget() { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Annotation a = Class.of(heap Widget()).getAnnotation(0);\n"
        "        if (a.getArgKind(0) != 3) { return 11; }\n"
        "        if (!a.getClassRef(\"value\").equals(\"Marker\")) { return 12; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"), 0);
}

// By-index inspection: kind tag, value, and the (empty) name of an unnamed arg.
TEST(ReflectionTests, annotationArgByIndex) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Annotation;\n"
        "@Order(7)\n"
        "public class Widget {\n"
        "    public Widget() { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Annotation a = Class.of(heap Widget()).getAnnotation(0);\n"
        "        if (a.getArgKind(0) != 0) { return 10; }\n"          // 0 = int64
        "        if (a.getArgInt(0) != (int64) 7) { return 11; }\n"
        "        if (a.getArgName(0).byteLength() != 0) { return 12; }\n" // unnamed
        "        return 0;\n"
        "    }\n"
        "}\n"), 0);
}

// Wrong-kind / absent-key reads return the typed fallbacks (no throw).
TEST(ReflectionTests, annotationArgWrongKindFallback) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Annotation;\n"
        "@Order(2)\n"
        "public class Widget {\n"
        "    public Widget() { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Annotation a = Class.of(heap Widget()).getAnnotation(0);\n"
        "        if (!a.getString(\"value\").equals(\"\")) { return 10; }\n" // int read as string
        "        if (a.getInt(\"missing\") != (int64) 0) { return 11; }\n"   // absent key
        "        if (a.getBool(\"value\")) { return 12; }\n"                 // int read as bool
        "        return 0;\n"
        "    }\n"
        "}\n"), 0);
}

// Field annotation argument: @Min(5) on a field -> getInt("value") == 5.
TEST(ReflectionTests, fieldAnnotationArg) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Field;\n"
        "import cajeta.reflect.Annotation;\n"
        "public class Widget {\n"
        "    @Min(5) public int32 v;\n"
        "    public Widget() { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Field f = Class.of(heap Widget()).getField(0);\n"
        "        Annotation a = f.getAnnotation(0);\n"
        "        if (a.getInt(\"value\") != (int64) 5) { return 11; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"), 0);
}

// Method annotation argument: @Order(3) on a method, found by scan.
TEST(ReflectionTests, methodAnnotationArg) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Method;\n"
        "import cajeta.reflect.Annotation;\n"
        "public class Widget {\n"
        "    public Widget() { return; }\n"
        "    @Order(3) public int32 ping() { return 1; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Class<?> c = Class.of(heap Widget());\n"
        "        int32 ok = 0;\n"
        "        int32 i = 0;\n"
        "        int32 n = c.getMethodCount();\n"
        "        while (i < n) {\n"
        "            Method m = c.getMethod(i);\n"
        "            if (m.getAnnotationCount() > 0) {\n"
        "                Annotation a = m.getAnnotation(0);\n"
        "                if (a.getInt(\"value\") == (int64) 3) { ok = ok + 1; }\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return ok;\n"   // exactly one method with @Order(3)
        "    }\n"
        "}\n"), 1);
}

// Multiple named arguments on one annotation, read independently by key.
TEST(ReflectionTests, annotationMultipleNamedArgs) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Annotation;\n"
        "@Range(min = 3, max = 9)\n"
        "public class Widget {\n"
        "    public Widget() { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Annotation a = Class.of(heap Widget()).getAnnotation(0);\n"
        "        if (a.getArgCount() != 2) { return 10; }\n"
        "        if (a.getInt(\"min\") != (int64) 3) { return 11; }\n"
        "        if (a.getInt(\"max\") != (int64) 9) { return 12; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"), 0);
}

// A bare annotation (no parens) reports zero arguments.
TEST(ReflectionTests, annotationNoArgsZeroCount) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Annotation;\n"
        "@Tracked\n"
        "public class Widget {\n"
        "    public Widget() { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        return Class.of(heap Widget()).getAnnotation(0).getArgCount();\n"
        "    }\n"
        "}\n"), 0);
}

// --- REFL-6b follow-ons: parameter arg values + list-valued args -------------

// A PARAMETER annotation's argument value is now captured (the formal-parameter
// parse path uses the shared parseAnnotationInstance). @Bound(min = 5) on a
// parameter -> getInt("min") == 5.
TEST(ReflectionTests, parameterAnnotationArg) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Method;\n"
        "import cajeta.reflect.Parameter;\n"
        "import cajeta.reflect.Annotation;\n"
        "public class Widget {\n"
        "    public Widget() { return; }\n"
        "    public int32 scaled(@Bound(min = 5) int32 factor) { return factor; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Class<?> c = Class.of(heap Widget());\n"
        "        int32 ok = 0;\n"
        "        int32 i = 0;\n"
        "        int32 n = c.getMethodCount();\n"
        "        while (i < n) {\n"
        "            Method m = c.getMethod(i);\n"
        "            if (m.getParameterCount() == 1) {\n"
        "                Parameter p = m.getParameter(0);\n"
        "                if (p.getAnnotationCount() == 1) {\n"
        "                    Annotation a = p.getAnnotation(0);\n"
        "                    if (a.getInt(\"min\") == (int64) 5) { ok = ok + 1; }\n"
        "                }\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return ok;\n"   // exactly one 1-arg method, its param @Bound(min=5)
        "    }\n"
        "}\n"), 1);
}

// String-list argument: @Tags({"a","b","c"}) -> StringList (kind 5), 3 elements.

// Int-list argument: @Sizes({1,2,3}) -> Int64List (kind 4).
TEST(ReflectionTests, annotationIntListArg) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Annotation;\n"
        "@Sizes({1, 2, 3})\n"
        "public class Widget {\n"
        "    public Widget() { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Annotation a = Class.of(heap Widget()).getAnnotation(0);\n"
        "        int32 idx = a.getArgIndex(\"value\");\n"
        "        if (a.getArgKind(idx) != 4) { return 11; }\n"
        "        if (a.getArgListCount(idx) != 3) { return 12; }\n"
        "        if (a.getArgListInt(idx, 0) != (int64) 1) { return 13; }\n"
        "        if (a.getArgListInt(idx, 2) != (int64) 3) { return 14; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"), 0);
}

// Bool-list argument: @Flags({true,false}) -> BoolList (kind 6).
TEST(ReflectionTests, annotationBoolListArg) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Annotation;\n"
        "@Flags({true, false})\n"
        "public class Widget {\n"
        "    public Widget() { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Annotation a = Class.of(heap Widget()).getAnnotation(0);\n"
        "        int32 idx = a.getArgIndex(\"value\");\n"
        "        if (a.getArgKind(idx) != 6) { return 11; }\n"
        "        if (a.getArgListCount(idx) != 2) { return 12; }\n"
        "        if (!a.getArgListBool(idx, 0)) { return 13; }\n"
        "        if (a.getArgListBool(idx, 1)) { return 14; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"), 0);
}

// --- REFL-7: template reflection (NOT generics — cajeta monomorphizes) --------
// Each instantiation (Box<int32>) is its own concrete class that retains both
// its declared template parameters (the <T>) and its concrete template
// arguments (int32). Type names render canonically the same way field/parameter
// type names do (e.g. "int32").

// A template instantiation reports its concrete template arguments.

// A template instantiation still carries its declared parameter (the <T>).

// A non-template class reports zero parameters and arguments.

// ---- REFL-8: Class.forName + registry -----------------------------------

// forName resolves a registered class by its canonical name to its Class.

// forName on an unknown name yields an empty Optional, not a crash.

// getName() then forName(name) round-trips back to the same class.

// A stdlib class is registered too — forName finds cajeta.lang.String.

// ---- REFL-12: bounded reflection (Class.heapInstance<T> / Class.forName<T>) --

// A reusable Shape hierarchy + an unrelated Animal for the subtype boundary.
#define REFL12_HIERARCHY \
    "package test;\n" \
    "import cajeta.reflect.Class;\n" \
    "import cajeta.lang.Optional;\n" \
    "import cajeta.lang.String;\n" \
    "public class Shape {\n" \
    "    public int32 sides;\n" \
    "    public Shape() { this.sides = 0; return; }\n" \
    "    public int32 sideCount() { return this.sides; }\n" \
    "}\n" \
    "public class Circle extends Shape {\n" \
    "    public Circle() { this.sides = 7; return; }\n" \
    "}\n" \
    "public class Animal {\n" \
    "    public int32 legs;\n" \
    "    public Animal() { this.legs = 4; return; }\n" \
    "}\n"

// Bounded heapInstance: a subtype name resolves, constructs, and the result is
// statically a Shape (no cast) — sideCount() dispatches to Circle's state (7).

// The exact-type bound is the degenerate case: heapInstance<Shape> of Shape
// itself resolves (identity counts as subtype) and runs Shape's ctor (sides=0).

// Boundary check: a name that resolves to a NON-subtype (Animal is not a Shape)
// yields empty — a clean not-found, not a bad cast / crash.

// An unknown name also yields empty (same path as unbounded forName).

// Bounded enumeration: subtypes<Shape>() is the closed-world closure — Shape
// itself + Circle (inclusive of the bound), and nothing unrelated (Animal).
TEST(ReflectionTests, boundedSubtypesClosure) {
    EXPECT_EQ(runCustomI32(
        REFL12_HIERARCHY
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Class<?>[] subs = Class.subtypes<Shape>();\n"
        "        return (int32) subs.count();\n"
        "    }\n"
        "}\n"), 2);
}

// A leaf with no subtypes returns just itself (identity counts as subtype).
TEST(ReflectionTests, boundedSubtypesLeafIsSelf) {
    EXPECT_EQ(runCustomI32(
        REFL12_HIERARCHY
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Class<?>[] subs = Class.subtypes<Animal>();\n"
        "        return (int32) subs.count();\n"
        "    }\n"
        "}\n"), 1);
}

// REFL-8 unblocks TemplateArgument.getType() for a class-typed argument:
// Box<Widget>'s argument resolves to the Widget Class.
TEST(ReflectionTests, templateArgGetTypeResolvesClass) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.TemplateArgument;\n"
        "public class Widget {\n"
        "    public Widget() { return; }\n"
        "}\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Box<Widget> b = heap Box<Widget>(heap Widget());\n"
        "        Class<?> c = Class.of(b);\n"
        "        Class<?> t = c.getTemplateArgument(0).getType();\n"
        "        if (!t.getName().equals(\"test.Widget\")) { return 12; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"), 0);
}

// A primitive template argument has no Class — getType() throws
// UnsupportedReflectionException (use getTypeName() instead).

// ---------------------------------------------------------------------------
// REFL-10 — package / annotation registry queries (Class.allClasses /
// classesInPackage / classesAnnotated). Each returns a Class[] of borrows over
// the process-wide registry; the tests scan the result by getName().
// ---------------------------------------------------------------------------

// allClasses() includes every registered class — find the fixture exactly once.

// classesInPackage("test") contains User; classesInPackage("cajeta.lang") does
// NOT contain User but DOES contain String (a stdlib class is registered too).
TEST(ReflectionTests, classesInPackageFilters) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "public class User {\n"
        "    public int32 id;\n"
        "    public User() { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Class<?>[] inTest = Class.classesInPackage(\"test\");\n"
        "        int32 a = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < (int32) inTest.count()) {\n"
        "            if (inTest[i].getName().equals(\"test.User\")) { a = a + 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        Class<?>[] inLang = Class.classesInPackage(\"cajeta.lang\");\n"
        "        int32 b = 0;\n"
        "        int32 c = 0;\n"
        "        i = 0;\n"
        "        while (i < (int32) inLang.count()) {\n"
        "            if (inLang[i].getName().equals(\"test.User\")) { b = b + 1; }\n"
        "            if (inLang[i].getName().equals(\"cajeta.lang.String\")) { c = c + 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (a == 1 && b == 0 && c >= 1) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"), 1);
}

// classesAnnotated("code.Marker") finds the @Marker class, not the plain one.

// REFL-12: classesAnnotated<@A>() token form — the annotation rides as a method
// type arg (resolved now that annotation decls register as types) and lowers to
// the string overload. Finds @Audited's class, not the plain one.
TEST(ReflectionTests, classesAnnotatedTokenForm) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "annotation Audited { }\n"
        "public class Plain {\n"
        "    public Plain() { return; }\n"
        "}\n"
        "@Audited\n"
        "public class Tagged {\n"
        "    public Tagged() { return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Class<?>[] tagged = Class.classesAnnotated<Audited>();\n"
        "        int32 found = 0;\n"
        "        int32 other = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < (int32) tagged.count()) {\n"
        "            if (tagged[i].getName().equals(\"test.Tagged\")) { found = found + 1; }\n"
        "            if (tagged[i].getName().equals(\"test.Plain\"))  { other = other + 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (found == 1 && other == 0) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"), 1);
}

// classesWithMethodAnnotated("code.Probe") is the bounded form of the
// allClasses() + per-method-filter discovery idiom (cajeta-unit's Runner):
// it matches a METHOD-level annotation only — neither a class-level @Probe
// nor a plain class qualifies.
TEST(ReflectionTests, classesWithMethodAnnotatedFilters) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "public class Plain {\n"
        "    public Plain() { return; }\n"
        "    public int32 f() { return 1; }\n"
        "}\n"
        "@Probe\n"
        "public class ClassTagged {\n"
        "    public ClassTagged() { return; }\n"
        "    public int32 f() { return 2; }\n"
        "}\n"
        "public class MethodTagged {\n"
        "    public MethodTagged() { return; }\n"
        "    @Probe public int32 f() { return 3; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Class<?>[] hits = Class.classesWithMethodAnnotated(\"code.Probe\");\n"
        "        int32 method = 0;\n"
        "        int32 wrong = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < (int32) hits.count()) {\n"
        "            if (hits[i].getName().equals(\"test.MethodTagged\")) { method = method + 1; }\n"
        "            if (hits[i].getName().equals(\"test.ClassTagged\")) { wrong = wrong + 1; }\n"
        "            if (hits[i].getName().equals(\"test.Plain\")) { wrong = wrong + 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (method == 1 && wrong == 0) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"), 1);
}

// ...and the token form classesWithMethodAnnotated<Probe>() lowers to the
// string overload exactly like classesAnnotated's.
TEST(ReflectionTests, classesWithMethodAnnotatedTokenForm) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "annotation Probe { }\n"
        "public class Plain {\n"
        "    public Plain() { return; }\n"
        "    public int32 f() { return 1; }\n"
        "}\n"
        "public class MethodTagged {\n"
        "    public MethodTagged() { return; }\n"
        "    @Probe public int32 f() { return 3; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Class<?>[] hits = Class.classesWithMethodAnnotated<Probe>();\n"
        "        int32 method = 0;\n"
        "        int32 wrong = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < (int32) hits.count()) {\n"
        "            if (hits[i].getName().equals(\"test.MethodTagged\")) { method = method + 1; }\n"
        "            if (hits[i].getName().equals(\"test.Plain\")) { wrong = wrong + 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (method == 1 && wrong == 0) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"), 1);
}

// ---------------------------------------------------------------------------
// REFL-11 — constant-fold statically-known reflection. The fold fires only for
// `Class.of(<final-class identifier>).<accessor>(...)`; these assert the folded
// path yields the same value the runtime path would (the fold elision itself is
// verified at the IR level separately). A non-final class declines the fold and
// must still work through the runtime path.
// ---------------------------------------------------------------------------

// Metadata fold: getFieldCount() over a final class -> compile-time constant.

// Field-load fold: getInt32(g, litIdx) over a final class -> direct field load.
TEST(ReflectionTests, foldFinalFieldLoad) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.reflect.Class;\n"
        "public final class Gadget {\n"
        "    public int32 a;\n"
        "    public int32 b;\n"
        "    public Gadget() { this.a = 7; this.b = 9; return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Gadget g = heap Gadget();\n"
        "        return Class.of(g).getInt32(g, 1);\n"
        "    }\n"
        "}\n"), 9);
}

// A non-final class declines the fold (subclass could shift layout) — the
// runtime reflective path still returns the correct value.

// The fold must NOT bypass visibility: a @Sealed final class's private field
// stays un-folded so the runtime IllegalAccessException still fires.

// REFL-1.7: the Modifiers value object reflects a class's packed flags.
// public final class -> isPublic && isFinal, NOT isStatic. (1 + 2 = 3.)
TEST(ReflectionTests, modifiersObjectClassFlags) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Modifiers;\n"
        "public final class Gadget {\n"
        "    public int32 a;\n"
        "    public Gadget() { this.a = 1; return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Gadget g = heap Gadget();\n"
        "        Modifiers m = Class.of(g).getModifiers();\n"
        "        int32 r = 0;\n"
        "        if (m.isPublic()) { r = r + 1; }\n"
        "        if (m.isFinal())  { r = r + 2; }\n"
        "        if (m.isStatic()) { r = r + 100; }\n"
        "        return r;\n"
        "    }\n"
        "}\n"), 3);
}

// REFL-1.7: a Field's Modifiers — a private field of a NON-sealed class is
// isPrivate (and not public). (4.)

// REFL-1.6: obj.getClass() returns the object's dynamic Class<?> (synthesized,
// no Object source edit). Field count off the RTTI = 2.

// REFL-1.5: T.class is the statically-known type's Class<T>. Field count = 3.

// REFL-1.5/1.6: T.class and obj.getClass() name the SAME #ClassObject — the
// per-type process-lifetime singleton. getInstanceSize matches on both.
TEST(ReflectionTests, classLiteralAndGetClassAgree) {
    EXPECT_EQ(runCustomI32(
        "package test;\n"
        "import cajeta.reflect.Class;\n"
        "public final class Gadget {\n"
        "    public int32 a;\n"
        "    public Gadget() { this.a = 1; return; }\n"
        "}\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Gadget g = heap Gadget();\n"
        "        Class<?>     viaObj = g.getClass();\n"
        "        Class<Gadget> viaLit = Gadget.class;\n"
        "        int64 a = viaObj.getInstanceSize();\n"
        "        int64 b = viaLit.getInstanceSize();\n"
        "        return (a == b) ? 1 : 0;\n"
        "    }\n"
        "}\n"), 1);
}
