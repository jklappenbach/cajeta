//
// #63 probe: multi-call interface dispatch with stack-Optional return.
// Bisects single-call vs two-call vs three-call shapes through an
// interface-typed receiver where the impl returns a stack Optional<T>.
//
// AsyncIteratorTests.singleCallViaInterface already pins single-call.
// This file isolates the minimum cardinality at which it fails and
// whether opt-assignment vs in-loop call is the trigger.
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

const char* kHeader =
    "package test;\n"
    "import cajeta.lang.Optional;\n"
    "import cajeta.threading.AsyncIterator;\n"
    "public final class TriIter implements AsyncIterator<int32> {\n"
    "    int32 i;\n"
    "    int32[] vals;\n"
    "    public TriIter() {\n"
    "        this.i = 0;\n"
    "        this.vals = new int32[3];\n"
    "        this.vals[0] = 10;\n"
    "        this.vals[1] = 20;\n"
    "        this.vals[2] = 30;\n"
    "    }\n"
    "    public Optional<int32> next() {\n"
    "        if (this.i >= 3) {\n"
    "            return stack Optional<int32>(false, 0);\n"
    "        }\n"
    "        int32 v = this.vals[this.i];\n"
    "        this.i = this.i + 1;\n"
    "        return stack Optional<int32>(true, v);\n"
    "    }\n"
    "}\n"
    "public final class D {\n";

} // namespace

// Single call through interface — baseline. Should match
// AsyncIteratorTests.singleCallViaInterface (different impl).
TEST(IfaceMultiCallProbe, singleCallViaInterface) {
    std::string src = std::string(kHeader) +
        "    public static int32 run() {\n"
        "        AsyncIterator<int32> iter = heap TriIter();\n"
        "        Optional<int32> o1 = iter.next();\n"
        "        if (o1.isPresent()) { return o1.get(); }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 10);
}

// Single call where the impl has no fields and returns a literal.
TEST(IfaceMultiCallProbe, singleCallViaInterfaceStatelessImpl) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.threading.AsyncIterator;\n"
        "public final class Stateless implements AsyncIterator<int32> {\n"
        "    public Optional<int32> next() {\n"
        "        return stack Optional<int32>(true, 42);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        AsyncIterator<int32> iter = heap Stateless();\n"
        "        Optional<int32> o = iter.next();\n"
        "        if (o.isPresent()) { return o.get(); }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Stateless impl, two sequential interface calls.
TEST(IfaceMultiCallProbe, twoCallsStatelessImpl) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.threading.AsyncIterator;\n"
        "public final class Stateless implements AsyncIterator<int32> {\n"
        "    public Optional<int32> next() {\n"
        "        return stack Optional<int32>(true, 42);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        AsyncIterator<int32> iter = heap Stateless();\n"
        "        Optional<int32> o1 = iter.next();\n"
        "        Optional<int32> o2 = iter.next();\n"
        "        int32 a = o1.get();\n"
        "        int32 b = o2.get();\n"
        "        return a + b;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 84);
}

// Stateful impl with one int field, interface receiver. Bisects whether
// "having a field" alone is enough vs reading from it in next().
TEST(IfaceMultiCallProbe, singleCallStatefulFieldNotRead) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.threading.AsyncIterator;\n"
        "public final class OneField implements AsyncIterator<int32> {\n"
        "    int32 i;\n"
        "    public OneField() { this.i = 5; }\n"
        "    public Optional<int32> next() {\n"
        "        return stack Optional<int32>(true, 42);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        AsyncIterator<int32> iter = heap OneField();\n"
        "        Optional<int32> o = iter.next();\n"
        "        if (o.isPresent()) { return o.get(); }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Stateful impl with one int field READ in next() and used as the value.
TEST(IfaceMultiCallProbe, singleCallStatefulFieldRead) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.threading.AsyncIterator;\n"
        "public final class OneFieldRead implements AsyncIterator<int32> {\n"
        "    int32 i;\n"
        "    public OneFieldRead() { this.i = 17; }\n"
        "    public Optional<int32> next() {\n"
        "        return stack Optional<int32>(true, this.i);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        AsyncIterator<int32> iter = heap OneFieldRead();\n"
        "        Optional<int32> o = iter.next();\n"
        "        if (o.isPresent()) { return o.get(); }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 17);
}

// Stateful impl with one int field WRITTEN in next(), value returned.
TEST(IfaceMultiCallProbe, singleCallStatefulFieldWrite) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.threading.AsyncIterator;\n"
        "public final class OneFieldWrite implements AsyncIterator<int32> {\n"
        "    int32 i;\n"
        "    public OneFieldWrite() { this.i = 0; }\n"
        "    public Optional<int32> next() {\n"
        "        this.i = this.i + 1;\n"
        "        return stack Optional<int32>(true, this.i);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        AsyncIterator<int32> iter = heap OneFieldWrite();\n"
        "        Optional<int32> o = iter.next();\n"
        "        if (o.isPresent()) { return o.get(); }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Concrete-receiver version of TriIter single-call — control.
TEST(IfaceMultiCallProbe, singleCallViaConcrete) {
    std::string src = std::string(kHeader) +
        "    public static int32 run() {\n"
        "        TriIter iter = heap TriIter();\n"
        "        Optional<int32> o1 = iter.next();\n"
        "        if (o1.isPresent()) { return o1.get(); }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 10);
}

// Two sequential calls through interface, both stored as locals.
TEST(IfaceMultiCallProbe, twoCallsSeparateLocals) {
    std::string src = std::string(kHeader) +
        "    public static int32 run() {\n"
        "        AsyncIterator<int32> iter = heap TriIter();\n"
        "        Optional<int32> o1 = iter.next();\n"
        "        Optional<int32> o2 = iter.next();\n"
        "        int32 a = o1.get();\n"
        "        int32 b = o2.get();\n"
        "        return a + b;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 30);
}

// Two calls — single reassigned local.
TEST(IfaceMultiCallProbe, twoCallsReassignSameLocal) {
    std::string src = std::string(kHeader) +
        "    public static int32 run() {\n"
        "        AsyncIterator<int32> iter = heap TriIter();\n"
        "        Optional<int32> o = iter.next();\n"
        "        int32 a = o.get();\n"
        "        o = iter.next();\n"
        "        int32 b = o.get();\n"
        "        return a + b;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 30);
}

// Same shape but with isPresent guard between calls.
TEST(IfaceMultiCallProbe, twoCallsReassignWithIsPresent) {
    std::string src = std::string(kHeader) +
        "    public static int32 run() {\n"
        "        AsyncIterator<int32> iter = heap TriIter();\n"
        "        int32 sum = 0;\n"
        "        Optional<int32> o = iter.next();\n"
        "        if (o.isPresent()) { sum = sum + o.get(); }\n"
        "        o = iter.next();\n"
        "        if (o.isPresent()) { sum = sum + o.get(); }\n"
        "        return sum;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 30);
}

// Loop unrolled to exactly three iterations — no `while`.
TEST(IfaceMultiCallProbe, threeCallsUnrolled) {
    std::string src = std::string(kHeader) +
        "    public static int32 run() {\n"
        "        AsyncIterator<int32> iter = heap TriIter();\n"
        "        int32 sum = 0;\n"
        "        Optional<int32> o = iter.next();\n"
        "        if (o.isPresent()) { sum = sum + o.get(); }\n"
        "        o = iter.next();\n"
        "        if (o.isPresent()) { sum = sum + o.get(); }\n"
        "        o = iter.next();\n"
        "        if (o.isPresent()) { sum = sum + o.get(); }\n"
        "        return sum;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 60);
}

// Same as above but with a fourth call expected to be empty.
TEST(IfaceMultiCallProbe, fourCallsUnrolledFourthEmpty) {
    std::string src = std::string(kHeader) +
        "    public static int32 run() {\n"
        "        AsyncIterator<int32> iter = heap TriIter();\n"
        "        int32 sum = 0;\n"
        "        Optional<int32> o = iter.next();\n"
        "        if (o.isPresent()) { sum = sum + o.get(); }\n"
        "        o = iter.next();\n"
        "        if (o.isPresent()) { sum = sum + o.get(); }\n"
        "        o = iter.next();\n"
        "        if (o.isPresent()) { sum = sum + o.get(); }\n"
        "        o = iter.next();\n"
        "        if (o.isPresent()) { sum = sum - 1000; }\n"
        "        return sum;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 60);
}
