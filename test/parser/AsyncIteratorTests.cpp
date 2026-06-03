//
// AsyncIterator<T> — minimal iteration contract for cajeta.threading.
// v1 ships the interface. The canonical iteration loop is:
//
//     Optional<T> opt = iter.next();
//     while (opt.isPresent()) {
//         T x = opt.get();
//         ...
//         opt = iter.next();
//     }
//
// Both concrete-typed and interface-typed receivers work. Interface
// dispatch required two coupled fixes (#63): the interface decl's
// prototype must adopt the sret ABI when its return type is a value-
// shape class (so the indirect call signature matches the impl), and
// the dispatch site must swap the iface body for the concrete data
// pointer at the `this` slot (sretOffset), not blindly at position 0
// — overwriting position 0 in the sret case clobbers the sret slot
// and the impl writes its result into garbage. A `for (T x in iter)`
// desugaring is the planned v2.1 surface.
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

} // namespace

// Pin (a) the interface shape, (b) virtual dispatch through a vtable,
// (c) stack-Optional return ABI — all three exercised by a single
// next() call where the receiver is interface-typed.
TEST(AsyncIteratorTests, singleCallViaInterface) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.threading.AsyncIterator;\n"
        "public final class FixedOne implements AsyncIterator<int32> {\n"
        "    public Optional<int32> next() {\n"
        "        return stack Optional<int32>(true, 7);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        AsyncIterator<int32> it = heap FixedOne();\n"
        "        Optional<int32> o = it.next();\n"
        "        if (o.isPresent()) { return o.get(); }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// The full iteration loop with the concrete receiver type. TriIter
// yields {10, 20, 30} then terminal-empty; the explicit
// `while (opt.isPresent())` form is the v1 user-side pattern that the
// deferred for-syntax will eventually desugar to.
TEST(AsyncIteratorTests, sumThreeViaConcreteReceiver) {
    auto src =
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
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        TriIter iter = heap TriIter();\n"
        "        int32 sum = 0;\n"
        "        Optional<int32> opt = iter.next();\n"
        "        while (opt.isPresent()) {\n"
        "            sum = sum + opt.get();\n"
        "            opt = iter.next();\n"
        "        }\n"
        "        return sum;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 60);
}

// Same TriIter, but the receiver-typed local is the interface, not
// the concrete class. Multi-call iface dispatch with stack-Optional
// return. Pinned as failing — repro for #63.
TEST(AsyncIteratorTests, sumThreeViaInterfaceLoop) {
    auto src =
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
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        AsyncIterator<int32> iter = heap TriIter();\n"
        "        int32 sum = 0;\n"
        "        Optional<int32> opt = iter.next();\n"
        "        int32 guard = 0;\n"
        "        while (opt.isPresent()) {\n"
        "            sum = sum + opt.get();\n"
        "            opt = iter.next();\n"
        "            guard = guard + 1;\n"
        "            if (guard > 10) { return -999; }\n"
        "        }\n"
        "        return sum;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 60);
}
