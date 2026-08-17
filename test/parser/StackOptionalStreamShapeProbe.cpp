//
// #66 regression guards — narrow shapes that exercise the sret-method
// fallback path in ReturnStatement.cpp (memcpy from a class-typed local's
// slot into the caller's sret slot, with the slot load step) and the
// inherited-from-sret-base detection in Method::returnsStackValue().
//
//   (O1) `return o;` where o is a stack-Optional<int32> local in an
//        sret-shaped method body. Statement.cpp's sret fallback must
//        load through the local's pointer slot to recover the struct
//        address before the memcpy, or the memcpy reads the slot's
//        pointer bytes instead of the struct contents and the caller
//        SIGSEGVs on the first method call.
//
//   (O2) `return stack Optional<int32>(false)` — the empty-case ctor.
//        (Was the two-arg `(false, null)` idiom, retired by title-tracking
//        Unit 1: a null arg can never match a primitive `T` formal, and the
//        no-matching-constructor hard error now rejects it.) Confirm the
//        stack form correctly zero-inits the primitive value slot.
//
//   (O3) Subclass overrides an sret-shaped base method even though its
//        own body has no `return stack X(...)` (e.g. PeekStream.next
//        is `return o;` only). Method::returnsStackValue() must walk
//        the superclass chain and force sret on the override to keep
//        vtable signatures aligned.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

void runWithRethrow(const std::string& src, int32_t expected) {
    try {
        EXPECT_EQ(runI32(src), expected);
    } catch (cajeta::Exception& e) {
        FAIL() << "cajeta::Exception: " << e.getMessage();
    } catch (const std::exception& e) {
        FAIL() << "std::exception: " << e.what();
    } catch (const char* s) {
        FAIL() << "const char*: " << s;
    } catch (...) {
        FAIL() << "unknown exception type";
    }
}

} // namespace

// O2 isolated: base Stream-shape returning `stack Optional<int32>(false)`
// from an explicit terminator. Smallest possible repro for the primitive-null
// stack-ctor path.
TEST(StackOptionalStreamShapeProbe, primitiveNullStackCtorReturnsEmpty) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public class Src {\n"
        "    public Optional<int32> next() {\n"
        "        return stack Optional<int32>(false);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Src s = heap Src();\n"
        "        Optional<int32> o = s.next();\n"
        "        if (o.isPresent()) {\n"
        "            return -1;\n"
        "        }\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    runWithRethrow(src, 42);
}

// O2 paired: present-path takes the stack ctor with a real primitive value,
// empty-path uses null. Mixed pattern verifies both branches of a typical
// next() body.

// O1 isolated, compile only: subclass overrides next() and ends with
// `return o;` where o is a local Optional<int32>. This is the FilterStream-
// shape exhausted-path return. Verifies Statement.cpp's sret-fallback
// memcpy from a local pointer to the sret slot.

// Trivially `return stack X(...)` — confirms baseline NRVO path is fine.
TEST(StackOptionalStreamShapeProbe, baselineDirectStackReturn) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public class S {\n"
        "    public Optional<int32> n() {\n"
        "        return stack Optional<int32>(true, 22);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        S s = heap S();\n"
        "        Optional<int32> r = s.n();\n"
        "        return r.get();\n"
        "    }\n"
        "}\n";
    runWithRethrow(src, 22);
}

// Even narrower: method has BOTH `return stack X(...)` (so the body scan
// sees it as sret) AND `return o;` of a class-typed local. No source.next
// chain — pure standalone Optional plays.
TEST(StackOptionalStreamShapeProbe, narrowSretReturnLocal) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public class S {\n"
        "    public Optional<int32> n(boolean b) {\n"
        "        if (b) {\n"
        "            return stack Optional<int32>(true, 11);\n"
        "        }\n"
        "        Optional<int32> o = stack Optional<int32>(true, 22);\n"
        "        return o;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        S s = heap S();\n"
        "        Optional<int32> r = s.n(false);\n"
        "        return r.get();\n"
        "    }\n"
        "}\n";
    runWithRethrow(src, 22);
}

TEST(StackOptionalStreamShapeProbe, returnLocalOptionalFromSretMethod) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public class Src {\n"
        "    int32 i;\n"
        "    public Src() { this.i = 0; }\n"
        "    public Optional<int32> next() {\n"
        "        if (this.i < 2) {\n"
        "            int32 v = this.i;\n"
        "            this.i = this.i + 1;\n"
        "            return stack Optional<int32>(true, v);\n"
        "        }\n"
        "        return stack Optional<int32>(false);\n"
        "    }\n"
        "}\n"
        "public class Filter {\n"
        "    Src source;\n"
        "    public Filter(Src s) { this.source #= s; }\n"
        "    public Optional<int32> next() {\n"
        "        Optional<int32> o = this.source.next();\n"
        "        while (o.isPresent()) {\n"
        "            int32 v = o.get();\n"
        "            if (v == 1) {\n"
        "                return stack Optional<int32>(true, v);\n"
        "            }\n"
        "            o = this.source.next();\n"
        "        }\n"
        "        return o;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Src s = heap Src();\n"
        "        Filter f = heap Filter(s);\n"
        "        Optional<int32> o = f.next();\n"
        "        if (!o.isPresent()) return -1;\n"
        "        if (o.get() != 1) return -2;\n"
        "        Optional<int32> empty = f.next();\n"
        "        if (empty.isPresent()) return -3;\n"
        "        return 7;\n"
        "    }\n"
        "}\n";
    runWithRethrow(src, 7);
}

// O1 + class hierarchy: Filter extends Src so the override question is
// real (vtable signature must align — both sret-shaped). Drives the same
// pattern through virtual dispatch.
TEST(StackOptionalStreamShapeProbe, virtualSretReturnLocalFromOverride) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public class Src {\n"
        "    public Optional<int32> next() {\n"
        "        return stack Optional<int32>(false);\n"
        "    }\n"
        "}\n"
        "public class Filter extends Src {\n"
        "    int32 i;\n"
        "    public Filter() { this.i = 0; }\n"
        "    public Optional<int32> next() {\n"
        "        if (this.i < 1) {\n"
        "            int32 v = this.i + 10;\n"
        "            this.i = this.i + 1;\n"
        "            return stack Optional<int32>(true, v);\n"
        "        }\n"
        "        Optional<int32> o = stack Optional<int32>(false);\n"
        "        return o;\n"  // return local through sret
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Src base = heap Filter();\n"
        "        Optional<int32> first = base.next();\n"
        "        if (!first.isPresent()) return -1;\n"
        "        if (first.get() != 10) return -2;\n"
        "        Optional<int32> empty = base.next();\n"
        "        if (empty.isPresent()) return -3;\n"
        "        return 99;\n"
        "    }\n"
        "}\n";
    runWithRethrow(src, 99);
}
