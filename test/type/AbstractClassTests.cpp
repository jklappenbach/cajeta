// Abstract classes and abstract methods (spec Classes §8.5).
//
// What works today, measured 2026-09-09 on 0.27.0 with `cajeta jit-run`:
// an abstract base with a concrete subclass dispatches correctly through
// a base-typed binding, a concrete class that inherits an abstract method
// without overriding it is rejected with
// CAJETA_ERROR_ABSTRACT_NOT_IMPLEMENTED, and an abstract method in one
// parent is satisfied by a concrete same-signature method in another
// (test/parser/MultiClassingPhase1Tests.cpp pins that case).
//
// Three checks are missing. Each test below is RED until one lands.
//
// 1. Allocating a class that still has an unimplemented abstract method
//    compiles. The empty vtable slot is called at run time and the
//    process takes SIGSEGV at a null fault address — measured with
//    `heap Shape()` where Shape declares `public abstract int32 area()`,
//    exit 139, "SIGSEGV caught — fault addr (nil)". The allocation site
//    is where this must be rejected.
// 2. An abstract method declared with a body is accepted and the body is
//    ignored.
// 3. A class that declares an abstract method is not required to carry
//    the `abstract` modifier, so a class that reads as concrete can hold
//    an empty slot.
//
// The expected codes below are named for what they check. Adjust them if
// the fix picks different ones.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <map>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

std::string compileExpectError(const std::string& src,
                               const std::string& expectCode) {
    try {
        CajetaJit::compile(src, "test.D");
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), expectCode);
        return e.getMessage();
    } catch (const std::exception& e) {
        return e.what();
    }
    ADD_FAILURE() << "expected a compile error";
    return "";
}

}  // namespace

// An abstract class is not instantiable. ENABLED 2026-09-20: the check
// now fires at the allocation site. Before it, this compiled and the call
// through the empty vtable slot took SIGSEGV at a null fault address.
TEST(AbstractClassTests, abstractClassIsNotInstantiable) {
    const char* src =
        "package test;\n"
        "public abstract class Shape {\n"
        "    public Shape() { return; }\n"
        "    public abstract int32 area();\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Shape s = heap Shape();\n"
        "        return s.area();\n"
        "    }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_ABSTRACT_INSTANTIATION");
}

// An abstract method declares a signature and no body.
TEST(AbstractClassTests, abstractMethodWithBodyRejected) {
    const char* src =
        "package test;\n"
        "public abstract class Shape {\n"
        "    public Shape() { return; }\n"
        "    public abstract int32 area() { return 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_ABSTRACT_METHOD_HAS_BODY");
}

// A class that declares an abstract method is itself abstract, and must
// say so.
TEST(AbstractClassTests, abstractMethodRequiresAbstractClass) {
    const char* src =
        "package test;\n"
        "public class Shape {\n"
        "    public Shape() { return; }\n"
        "    public abstract int32 area();\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_ABSTRACT_METHOD_IN_CONCRETE_CLASS");
}

// --- the other half: the cases that must NOT fire -------------------------
//
// A check that only ever fires is half a check. CLAUDE.md §5: "a check needs
// tests that assert it FIRES and tests that assert it does NOT", and a
// predicate that silently disabled a whole check once read as a clean run
// for an hour. An abstract type is perfectly legal nearly everywhere — it is
// ALLOCATION that is not — so each of these would be a regression if the
// rejection widened.

namespace {

void compileOk(const std::string& src) {
    try {
        auto jit = CajetaJit::compile(src, "test.D");
        EXPECT_NE(jit, nullptr);
    } catch (cajeta::Exception& e) {
        ADD_FAILURE() << "must compile, got " << e.getErrorId() << ": "
                      << e.getMessage();
    }
}

// One abstract base, one concrete subclass, reused by every case below.
const char* BASE =
    "package test;\n"
    "public abstract class Shape {\n"
    "    public Shape() { return; }\n"
    "    public abstract int32 area();\n"
    "}\n"
    "public class Square extends Shape {\n"
    "    public Square() { return; }\n"
    "    public int32 area() { return 4; }\n"
    "}\n";

}  // namespace

// The concrete subclass allocates. If this broke, the check would be reading
// inherited abstract methods rather than declared ones.
TEST(AbstractClassTests, aConcreteSubclassStillAllocates) {
    compileOk(std::string(BASE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Square s #= heap Square();\n"
        "        return s.area();\n"
        "    }\n"
        "}\n");
}

// An abstract type as a PARAMETER — the whole point of an abstract base.
TEST(AbstractClassTests, anAbstractTypeIsLegalAsAParameter) {
    compileOk(std::string(BASE) +
        "public final class D {\n"
        "    static int32 areaOf(Shape s) { return s.area(); }\n"
        "    public static int32 run() {\n"
        "        Square q #= heap Square();\n"
        "        return D.areaOf(q);\n"
        "    }\n"
        "}\n");
}

// As a LOCAL's declared type, bound to a concrete instance.
TEST(AbstractClassTests, anAbstractTypeIsLegalAsALocalBinding) {
    compileOk(std::string(BASE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Shape s #= heap Square();\n"
        "        return s.area();\n"
        "    }\n"
        "}\n");
}

// As a FIELD, and as a RETURN type.
TEST(AbstractClassTests, anAbstractTypeIsLegalAsAFieldAndAReturn) {
    compileOk(std::string(BASE) +
        "public final class Holder {\n"
        "    Shape held;\n"
        "    public Holder() { this.held = null; }\n"
        "    public void put(Shape s) { this.held #= s; return; }\n"
        "    public Shape get() { return this.held; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h #= heap Holder();\n"
        "        Square q #= heap Square();\n"
        "        h.put(#q);\n"
        "        return h.get().area();\n"
        "    }\n"
        "}\n");
}

// An ARRAY of an abstract type allocates SLOTS, not instances — `heap
// Shape[4]` is four null references and calls nothing, so the rejection
// must not reach it. This is the case most likely to be caught by a
// too-broad check, because it goes through the same NewExpression.
TEST(AbstractClassTests, anArrayOfAnAbstractTypeIsLegal) {
    compileOk(std::string(BASE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Shape[] all #= heap Shape[4];\n"
        "        all[0] #= heap Square();\n"
        "        return all[0].area();\n"
        "    }\n"
        "}\n");
}

// --- abstract-classes plan Unit 0: the modifier is stored --------------------

namespace {

int32_t runAbstractI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    if (!jit) { ADD_FAILURE() << "compile returned null"; return -1; }
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// The class modifier reaches reflection: Shape says abstract, Square does not.
TEST(AbstractClassTests, abstractClassReflectsIsAbstract) {
    EXPECT_EQ(runAbstractI32(
        "package test;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Modifiers;\n"
        "import cajeta.lang.Optional;\n"
        "public abstract class Shape {\n"
        "    public Shape() { return; }\n"
        "    public abstract int32 area();\n"
        "}\n"
        "public class Square extends Shape {\n"
        "    public Square() { return; }\n"
        "    public int32 area() { return 4; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 r = 0;\n"
        "        Optional<Class<?>> os #= Class.forName(\"test.Shape\");\n"
        "        if (os.isPresent()) {\n"
        "            Class<?> s = os.get();\n"
        "            if (s.isAbstract()) { r = r + 1; }\n"
        "            Modifiers m #= s.getModifiers();\n"
        "            if (m.isAbstract()) { r = r + 2; }\n"
        "        }\n"
        "        Square q = heap Square();\n"
        "        if (Class.of(q).isAbstract()) { r = r + 100; }\n"
        "        return r;\n"
        "    }\n"
        "}\n"), 3);
}

// Records stay concrete no-vtable value types.
TEST(AbstractClassTests, abstractRecordRejected) {
    compileExpectError(
        "package test;\n"
        "public abstract record R { int32 x; }\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n",
        "CAJETA_ERROR_RECORD_ABSTRACT");
}

// --- Unit 1: the method modifier -------------------------------------------

// A missing body is not a way to spell abstract. The must-not-fire side is
// the embedded stdlib, whose @Native and @Intrinsic methods are bodiless and
// compile in every test here.
TEST(AbstractClassTests, bodilessMethodWithoutKeywordRejected) {
    compileExpectError(
        "package test;\n"
        "public abstract class Shape {\n"
        "    public Shape() { return; }\n"
        "    public int32 area();\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n",
        "CAJETA_ERROR_METHOD_MISSING_BODY");
}

TEST(AbstractClassTests, abstractStaticRejected) {
    compileExpectError(
        "package test;\n"
        "public abstract class Shape {\n"
        "    public Shape() { return; }\n"
        "    public static abstract int32 count();\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n",
        "CAJETA_ERROR_ABSTRACT_STATIC_METHOD");
}

TEST(AbstractClassTests, abstractPrivateRejected) {
    compileExpectError(
        "package test;\n"
        "public abstract class Shape {\n"
        "    public Shape() { return; }\n"
        "    private abstract int32 area();\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n",
        "CAJETA_ERROR_ABSTRACT_PRIVATE_METHOD");
}

// On an interface method the keyword is redundant and accepted.
TEST(AbstractClassTests, abstractOnInterfaceMethodAccepted) {
    EXPECT_EQ(runAbstractI32(
        "package test;\n"
        "public interface Drawable {\n"
        "    abstract int32 draw();\n"
        "}\n"
        "public class Sprite implements Drawable {\n"
        "    public Sprite() { return; }\n"
        "    public int32 draw() { return 42; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Drawable d = heap Sprite();\n"
        "        return d.draw();\n"
        "    }\n"
        "}\n"), 42);
}

// The method modifier reaches reflection too.
TEST(AbstractClassTests, abstractMethodReflectsIsAbstract) {
    EXPECT_EQ(runAbstractI32(
        "package test;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Method;\n"
        "import cajeta.reflect.Modifiers;\n"
        "import cajeta.lang.Optional;\n"
        "public abstract class Shape {\n"
        "    public Shape() { return; }\n"
        "    public abstract int32 area();\n"
        "    public int32 twice() { return 2; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 r = 0;\n"
        "        Optional<Class<?>> os #= Class.forName(\"test.Shape\");\n"
        "        if (!os.isPresent()) { return -1; }\n"
        "        Class<?> s = os.get();\n"
        "        int32 n = s.getMethodCount();\n"
        "        int32 i = 0;\n"
        "        while (i < n) {\n"
        "            Method m #= s.getMethod(i);\n"
        "            Modifiers md #= m.getModifiers();\n"
        "            if (m.getName().equals(\"test.Shape::area(pointer)\") && md.isAbstract()) { r = r + 1; }\n"
        "            if (m.getName().equals(\"test.Shape::twice(pointer)\") && md.isAbstract()) { r = r + 100; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return r;\n"
        "    }\n"
        "}\n"), 1);
}

// --- Unit 2: the class modifier ---------------------------------------------

// A complete abstract class is still not allocatable, by either allocator.
TEST(AbstractClassTests, completeAbstractClassRefusedAtHeap) {
    compileExpectError(
        "package test;\n"
        "public abstract class Base {\n"
        "    public Base() { return; }\n"
        "    public int32 id() { return 7; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { Base b = heap Base(); return b.id(); }\n"
        "}\n",
        "CAJETA_ERROR_ABSTRACT_INSTANTIATION");
}

TEST(AbstractClassTests, completeAbstractClassRefusedAtStack) {
    compileExpectError(
        "package test;\n"
        "public abstract class Base {\n"
        "    public Base() { return; }\n"
        "    public int32 id() { return 7; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { Base b = stack Base(); return b.id(); }\n"
        "}\n",
        "CAJETA_ERROR_ABSTRACT_INSTANTIATION");
}

// An intermediate abstract class leaves area() to its leaf without restating
// it, and the leaf dispatches through the intermediate binding.
TEST(AbstractClassTests, intermediateAbstractClassNeedNotRestate) {
    EXPECT_EQ(runAbstractI32(
        "package test;\n"
        "public abstract class Shape {\n"
        "    public Shape() { return; }\n"
        "    public abstract int32 area();\n"
        "}\n"
        "public abstract class Polygon extends Shape {\n"
        "    int32 sides;\n"
        "    public Polygon(int32 n) { this.sides = n; }\n"
        "    public int32 sideCount() { return this.sides; }\n"
        "}\n"
        "public class Square extends Polygon {\n"
        "    int32 side;\n"
        "    public Square(int32 s) { super(4); this.side = s; }\n"
        "    public int32 area() { return this.side * this.side; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Polygon p = heap Square(3);\n"
        "        return p.area() + p.sideCount();\n"
        "    }\n"
        "}\n"), 13);
}

// An abstract class implements an interface and leaves draw() to its leaf.
// The call through the interface binding reaches the leaf.
TEST(AbstractClassTests, abstractClassLeavesInterfaceMethodToLeaf) {
    EXPECT_EQ(runAbstractI32(
        "package test;\n"
        "public interface Drawable { int32 draw(); int32 id(); }\n"
        "public abstract class Widget implements Drawable {\n"
        "    public Widget() { return; }\n"
        "    public int32 id() { return 1; }\n"
        "}\n"
        "public class Button extends Widget {\n"
        "    public Button() { return; }\n"
        "    public int32 draw() { return 41; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Drawable d = heap Button();\n"
        "        return d.draw() + d.id();\n"
        "    }\n"
        "}\n"), 42);
}

// The exemption is the abstract class's alone: a concrete leaf under it
// still owes every obligation, from the base and from the interface.
TEST(AbstractClassTests, concreteLeafStillOwesInheritedAbstract) {
    compileExpectError(
        "package test;\n"
        "public abstract class Shape {\n"
        "    public Shape() { return; }\n"
        "    public abstract int32 area();\n"
        "}\n"
        "public abstract class Polygon extends Shape {\n"
        "    public Polygon() { return; }\n"
        "}\n"
        "public class Blob extends Polygon {\n"
        "    public Blob() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n",
        "CAJETA_ERROR_ABSTRACT_NOT_IMPLEMENTED");
}

TEST(AbstractClassTests, concreteLeafStillOwesInterfaceMethod) {
    compileExpectError(
        "package test;\n"
        "public interface Drawable { int32 draw(); }\n"
        "public abstract class Widget implements Drawable {\n"
        "    public Widget() { return; }\n"
        "}\n"
        "public class Button extends Widget {\n"
        "    public Button() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n",
        "CAJETA_ERROR_INTERFACE_NOT_IMPLEMENTED");
}

TEST(AbstractClassTests, abstractFinalClassRejected) {
    compileExpectError(
        "package test;\n"
        "public abstract final class Base {\n"
        "    public Base() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n",
        "CAJETA_ERROR_ABSTRACT_FINAL_CLASS");
}

// An abstract class template instantiates abstract.
TEST(AbstractClassTests, abstractTemplateInstantiationRefused) {
    compileExpectError(
        "package test;\n"
        "public abstract class Box<T> {\n"
        "    public Box() { return; }\n"
        "    public abstract T get();\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { Box<int32> b = heap Box<int32>(); return 0; }\n"
        "}\n",
        "CAJETA_ERROR_ABSTRACT_INSTANTIATION");
}

// The abstract base lives in another source module.
TEST(AbstractClassTests, abstractBaseFromAnotherSource) {
    std::map<std::string, std::string> sources = {
        {"lib.Shape",
         "package lib;\n"
         "public abstract class Shape {\n"
         "    public Shape() { return; }\n"
         "    public abstract int32 area();\n"
         "    public int32 twice() { return this.area() * 2; }\n"
         "}\n"},
        {"test.D",
         "package test;\n"
         "import lib.Shape;\n"
         "public class Square extends Shape {\n"
         "    public Square() { return; }\n"
         "    public int32 area() { return 9; }\n"
         "}\n"
         "public final class D {\n"
         "    public static int32 run() { Shape s = heap Square(); return s.twice(); }\n"
         "}\n"},
    };
    auto jit = CajetaJit::compile(sources, "test.D");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 18);
}

// A mock of an abstract class supplies the abstract method and allocates.
TEST(AbstractClassTests, generateMockOfAbstractClassAllocates) {
    std::map<std::string, std::string> sources = {
        {"dev.cajeta.unit.MockEngine",
         "package dev.cajeta.unit;\n"
         "import cajeta.lang.Int32;\n"
         "public class MockEngine {\n"
         "    public int32 calls;\n"
         "    public MockEngine() { this.calls = 0; }\n"
         "    public Object handle(String name, #Object[] a) {\n"
         "        this.calls = this.calls + 1;\n"
         "        return Int32.of(42);\n"
         "    }\n"
         "}\n"},
        {"test.Gateway",
         "package test;\n"
         "@GenerateMock\n"
         "public abstract class Gateway {\n"
         "    public Gateway() { }\n"
         "    public abstract int32 fetch(int32 k);\n"
         "}\n"},
        {"test.D",
         "package test;\n"
         "public final class D {\n"
         "    public static int32 run() {\n"
         "        MockGateway m = heap MockGateway();\n"
         "        Gateway g = m;\n"
         "        return g.fetch(5);\n"
         "    }\n"
         "}\n"},
    };
    auto jit = CajetaJit::compile(sources, "test.D");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 42);
}

// The interface table follows the class hierarchy without an abstract class
// in it: a leaf under a concrete implementor converts and dispatches.
TEST(AbstractClassTests, leafUnderConcreteImplementorDispatchesThroughInterface) {
    EXPECT_EQ(runAbstractI32(
        "package test;\n"
        "public interface Drawable { int32 draw(); }\n"
        "public class Widget implements Drawable {\n"
        "    public Widget() { return; }\n"
        "    public int32 draw() { return 5; }\n"
        "}\n"
        "public class Button extends Widget {\n"
        "    public Button() { return; }\n"
        "    public int32 draw() { return 6; }\n"
        "}\n"
        "public class Plain extends Widget {\n"
        "    public Plain() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Drawable a = heap Button();\n"
        "        Drawable b = heap Plain();\n"
        "        return a.draw() * 10 + b.draw();\n"
        "    }\n"
        "}\n"), 65);
}

// Three levels: the implementation sits two bases up.
TEST(AbstractClassTests, grandchildFindsImplementationTwoBasesUp) {
    EXPECT_EQ(runAbstractI32(
        "package test;\n"
        "public interface Drawable { int32 draw(); int32 id(); }\n"
        "public abstract class Widget implements Drawable {\n"
        "    public Widget() { return; }\n"
        "    public int32 id() { return 3; }\n"
        "}\n"
        "public abstract class Control extends Widget {\n"
        "    public Control() { return; }\n"
        "}\n"
        "public class Button extends Control {\n"
        "    public Button() { return; }\n"
        "    public int32 draw() { return 40; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Drawable d = heap Button();\n"
        "        return d.draw() + d.id();\n"
        "    }\n"
        "}\n"), 43);
}

// --- Unit 3: nothing reaches a null slot ------------------------------------

// Bounded reflection refuses the abstract class and admits the concrete one.
TEST(AbstractClassTests, reflectiveAllocationRefusesAbstractClass) {
    EXPECT_EQ(runAbstractI32(
        "package test;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.lang.Optional;\n"
        "public abstract class Shape {\n"
        "    public Shape() { return; }\n"
        "    public abstract int32 area();\n"
        "}\n"
        "public class Square extends Shape {\n"
        "    public Square() { return; }\n"
        "    public int32 area() { return 4; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 r = 0;\n"
        "        Optional<Shape> a #= Class.heapInstance<Shape>(\"test.Shape\");\n"
        "        if (!a.isPresent()) { r = r + 1; }\n"
        "        Optional<Shape> b #= Class.heapInstance<Shape>(\"test.Square\");\n"
        "        if (b.isPresent()) { r = r + 2 + b.get().area(); }\n"
        "        return r;\n"
        "    }\n"
        "}\n"), 7);
}

// The raw allocation primitive bypasses the bound check. A call on the result
// lands on the abstract slot's stub, which raises, so the process neither
// faults nor returns a value from nothing.
TEST(AbstractClassTests, abstractSlotStubRaisesInsteadOfFaulting) {
    EXPECT_EQ(runAbstractI32(
        "package test;\n"
        "import cajeta.reflect.Class;\n"
        "import cajeta.lang.Optional;\n"
        "public abstract class Shape {\n"
        "    public Shape() { return; }\n"
        "    public abstract int32 area();\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Optional<Class<?>> oc #= Class.forName(\"test.Shape\");\n"
        "        if (!oc.isPresent()) { return -1; }\n"
        "        Class<?> c = oc.get();\n"
        "        Shape s #= (Shape) c.heapInstance(0);\n"
        "        try {\n"
        "            return s.area();\n"
        "        } catch (Throwable t) {\n"
        "            return 42;\n"
        "        }\n"
        "    }\n"
        "}\n"), 42);
}
