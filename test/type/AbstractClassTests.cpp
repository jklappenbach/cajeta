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
TEST(AbstractClassTests, DISABLED_abstractMethodWithBodyRejected) {
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
TEST(AbstractClassTests, DISABLED_abstractMethodRequiresAbstractClass) {
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
