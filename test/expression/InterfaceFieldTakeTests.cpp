// Moving an owned value out of an interface-typed slot decays the slot's kind tag
// to borrowed, so the receiver owns it alone. Class-typed twins pin the reference.
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

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) {
        s.replace(p, from.size(), to);
    }
}

// A holder whose field is typed `FT` (Shape or Box) with the given `take()` body. The
// holder dies inside release(), the value is read after a churn allocation, and run()
// returns leaked * 1000 + (live objects held by work) * 100 + the value read.
std::string takeProgram(const std::string& fieldType, const std::string& takeBody) {
    std::string src =
        "package test;\n"
        "public interface Shape {\n"
        "    int32 size();\n"
        "}\n"
        "public class Box implements Shape {\n"
        "    public int32 v;\n"
        "    public Box(int32 v) { this.v = v; }\n"
        "    public int32 size() { return this.v; }\n"
        "}\n"
        "public class H {\n"
        "    public FT s;\n"
        "    public H() { this.s = null; }\n"
        "    public void put(#FT x) { this.s #= x; }\n"
        "    public #FT take() { TAKE }\n"
        "}\n"
        "public final class D {\n"
        "    static #FT release() {\n"
        "        H h = heap H();\n"
        "        h.put(#heap Box(7));\n"
        "        FT t #= h.take();\n"
        "        return #t;\n"
        "    }\n"
        "    static int32 work(int64 base) {\n"
        "        FT a #= D.release();\n"
        "        Box churn = heap Box(99);\n"
        "        int64 held = Cajeta.liveCount() - base;\n"
        "        return (int32) (held * 100) + a.size();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = D.work(base);\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 1000) + t;\n"
        "    }\n"
        "}\n";
    replaceAll(src, "TAKE", takeBody);
    replaceAll(src, "FT", fieldType);
    return src;
}

const char* kReturnSharpField = "return #this.s;";
const char* kBindThenReturn = "FT t #= this.s; return #t;";
const char* kBindThenNullStore = "FT t #= this.s; this.s = null; return #t;";
const char* kBindThenSharpNullStore = "FT t #= this.s; this.s #= null; return #t;";

} // namespace

// `return #this.s` on an interface field: the value outlives the holder and is freed once.
TEST(InterfaceFieldTakeTests, returnSharpFieldTransfersTitle) {
    EXPECT_EQ(runI32(takeProgram("Shape", kReturnSharpField)), 207);
}

// `t #= this.s; return #t` on an interface field.
TEST(InterfaceFieldTakeTests, bindThenReturnTransfersTitle) {
    EXPECT_EQ(runI32(takeProgram("Shape", kBindThenReturn)), 207);
}

// A null store after the take neither frees the taken value nor clears the local.
TEST(InterfaceFieldTakeTests, nullStoreAfterTakeKeepsValue) {
    EXPECT_EQ(runI32(takeProgram("Shape", kBindThenNullStore)), 207);
}

// `this.s #= null` after the take behaves like the plain null store.
TEST(InterfaceFieldTakeTests, sharpNullStoreAfterTakeKeepsValue) {
    EXPECT_EQ(runI32(takeProgram("Shape", kBindThenSharpNullStore)), 207);
}

// Class-typed twins of the four shapes above: the reference behaviour.
TEST(InterfaceFieldTakeTests, classFieldTwinsTransferTitle) {
    EXPECT_EQ(runI32(takeProgram("Box", kReturnSharpField)), 207);
    EXPECT_EQ(runI32(takeProgram("Box", kBindThenReturn)), 207);
    EXPECT_EQ(runI32(takeProgram("Box", kBindThenNullStore)), 207);
    EXPECT_EQ(runI32(takeProgram("Box", kBindThenSharpNullStore)), 207);
}

// A value taken out of the field goes back in with `put(#a)`, stays readable, and the
// holder's teardown frees it exactly once.
TEST(InterfaceFieldTakeTests, takenValuePutBackIsOwnedByHolder) {
    std::string src =
        "package test;\n"
        "public interface Shape {\n"
        "    int32 size();\n"
        "}\n"
        "public class Box implements Shape {\n"
        "    public int32 v;\n"
        "    public Box(int32 v) { this.v = v; }\n"
        "    public int32 size() { return this.v; }\n"
        "}\n"
        "public class H {\n"
        "    public Shape s;\n"
        "    public H() { this.s = null; }\n"
        "    public void put(#Shape x) { this.s #= x; }\n"
        "    public #Shape take() { Shape t #= this.s; this.s = null; return #t; }\n"
        "}\n"
        "public final class D {\n"
        "    static int32 work(int64 base) {\n"
        "        H h = heap H();\n"
        "        h.put(#heap Box(7));\n"
        "        Shape a #= h.take();\n"
        "        Box churn = heap Box(99);\n"
        "        if (a.size() != 7) { return -91; }\n"
        "        h.put(#a);\n"
        "        Box churn2 = heap Box(98);\n"
        "        int64 held = Cajeta.liveCount() - base;\n"
        "        return (int32) (held * 100) + h.s.size();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = D.work(base);\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 1000) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 407);
}

// A take from a field that only borrows forwards the borrow: the lender frees it once.
TEST(InterfaceFieldTakeTests, takeFromBorrowedFieldYieldsBorrow) {
    std::string src =
        "package test;\n"
        "public interface Shape {\n"
        "    int32 size();\n"
        "}\n"
        "public class Box implements Shape {\n"
        "    public int32 v;\n"
        "    public Box(int32 v) { this.v = v; }\n"
        "    public int32 size() { return this.v; }\n"
        "}\n"
        "public class H {\n"
        "    public Shape s;\n"
        "    public H() { this.s = null; }\n"
        "    public void lend(Shape x) { this.s #= x; }\n"
        "}\n"
        "public final class D {\n"
        "    static int32 work(int64 base) {\n"
        "        Shape mine = heap Box(8);\n"
        "        {\n"
        "            H h = heap H();\n"
        "            h.lend(mine);\n"
        "            Shape a #= #h.s;\n"
        "            if (a.size() != 8) { return -91; }\n"
        "        }\n"
        "        Box churn = heap Box(99);\n"
        "        int64 held = Cajeta.liveCount() - base;\n"
        "        return (int32) (held * 100) + mine.size();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = D.work(base);\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 1000) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 208);
}

// `#S.g` out of an interface-typed static: the local owns it and a later null store
// of the static does not free it.
TEST(InterfaceFieldTakeTests, staticFieldTakeTransfersTitle) {
    std::string src =
        "package test;\n"
        "public interface Shape {\n"
        "    int32 size();\n"
        "}\n"
        "public class Box implements Shape {\n"
        "    public int32 v;\n"
        "    public Box(int32 v) { this.v = v; }\n"
        "    public int32 size() { return this.v; }\n"
        "}\n"
        "public class S {\n"
        "    public static Shape g;\n"
        "}\n"
        "public final class D {\n"
        "    static int32 work(int64 base) {\n"
        "        S.g #= heap Box(5);\n"
        "        Shape a #= #S.g;\n"
        "        S.g = null;\n"
        "        Box churn = heap Box(99);\n"
        "        int64 held = Cajeta.liveCount() - base;\n"
        "        return (int32) (held * 100) + a.size();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = D.work(base);\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 1000) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 205);
}

// `#arr[i]` out of an interface array: a later null store of the element does not
// clear the taken local.
TEST(InterfaceFieldTakeTests, arrayElementTakeSurvivesNullStore) {
    std::string src =
        "package test;\n"
        "public interface Shape {\n"
        "    int32 size();\n"
        "}\n"
        "public class Box implements Shape {\n"
        "    public int32 v;\n"
        "    public Box(int32 v) { this.v = v; }\n"
        "    public int32 size() { return this.v; }\n"
        "}\n"
        "public final class D {\n"
        "    static int32 work(int64 base) {\n"
        "        Shape[] arr = heap Shape[2];\n"
        "        arr[0] #= heap Box(3);\n"
        "        Shape a #= #arr[0];\n"
        "        arr[0] = null;\n"
        "        Box churn = heap Box(99);\n"
        "        int64 held = Cajeta.liveCount() - base;\n"
        "        return (int32) (held * 100) + a.size();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = D.work(base);\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 1000) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 303);
}
