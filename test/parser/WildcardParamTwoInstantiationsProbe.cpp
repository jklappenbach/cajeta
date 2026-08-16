//
// fix/tour — regression pin for the WildcardsDemo SIGSEGV investigation.
//
// The crash was NOT a wildcard/dispatch bug: `heap Box<Dog>(heap Dog())`
// passes an OWNED TEMPORARY to a ctor that borrow-stores (`this.value = v`),
// so the temp drops at ctor exit and `value` dangles — the documented
// deferred-soundness gap (borrow field-stores are accepted until reference
// types land; safety is interim lint + debug-runtime). The wildcard, the
// second subclass, the qualified call form, and the chained inline dispatch
// were all red herrings: any of those shapes crashes with the inline temp and
// passes with a local-bound argument.
//
// These tests pin the SOUND shapes: multiple concrete instantiations through
// one bounded-wildcard parameter, both call forms, chained inline dispatch.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
const char* kSrc =
    "package test;\n"
    "public class Animal {\n"
    "    public int32 tag() { return 1; }\n"
    "}\n"
    "public class Dog extends Animal {\n"
    "    public int32 tag() { return 2; }\n"
    "}\n"
    "public class Cat extends Animal {\n"
    "    public int32 tag() { return 3; }\n"
    "}\n"
    "public class Box<T> {\n"
    "    T value;\n"
    "    public Box(T v) { this.value #= v; }\n"
    "}\n"
    "public final class D {\n"
    "    public static int32 inspect(Box<? extends Animal> b) {\n"
    "        return b.value.tag();\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        Dog d = heap Dog();\n"
    "        Cat c = heap Cat();\n"
    "        Box<Dog> bDog = heap Box<Dog>(d);\n"
    "        Box<Cat> bCat = heap Box<Cat>(c);\n"
    "        int32 viaQualified = D.inspect(bDog) * 10 + D.inspect(bCat);\n"
    "        int32 viaBare = inspect(bDog) * 10 + inspect(bCat);\n"
    "        if (viaQualified == viaBare) { return viaQualified; }\n"
    "        return -1;\n"
    "    }\n"
    "}\n";
} // namespace

// Two concrete instantiations through one wildcard param, both call forms,
// chained `b.value.tag()` dispatch: Dog=2, Cat=3 -> 23.
TEST(WildcardParamTwoInstantiationsProbe, twoInstantiationsBothDispatch) {
    auto jit = CajetaJit::compile(kSrc, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 23);
}
