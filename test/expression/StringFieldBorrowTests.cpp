// field-store-ownership Unit 6 (spec 5): a String field stored with `=` is a borrow, like every
// other type, and `#= x.clone()` is how code keeps its own copy.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& workBody) {
    return "package test;\n"
           "public class K {\n"
           "    public String v;\n"
           "    public K() { this.v = \"\"; }\n"
           "    public void keep(^String s) { this.v = s; }\n"
           "    public void keepSharp(String s) { this.v #= s; }\n"
           "    public void keepField(^K o) { this.v = o.v; }\n"
           "    public void keepClone(^K o) { this.v #= o.v.clone(); }\n"
           "    public void keepConcat(^String a, ^String b) { this.v = a + b; }\n"
           "    public void keepCall() { this.v = Ut.plainString(40); }\n"
           "    public void keepView(^K o) { this.v = o.peek(); }\n"
           "    public String peek() { return this.v; }\n"
           "}\n"
           "public class C {\n"
           "    public String v;\n"
           "    public C(^String s) { this.v = s; }\n"
           "}\n"
           "public class Box<T> {\n"
           "    public T v;\n"
           "    public Box() { }\n"
           "    public void put(^T x) { this.v = x; }\n"
           "}\n"
           "public final class Ut {\n"
           "    public static #String heapString(int32 n) {\n"
           "        int8[] buf = Cajeta.allocBytes((int64) n);\n"
           "        int32 i = 0;\n"
           "        while (i < n) {\n"
           "            buf[i] = (int8) (97 + (i - (i / 26) * 26));\n"
           "            i = i + 1;\n"
           "        }\n"
           "        return heap String(#buf, n);\n"
           "    }\n"
           "    public static String plainString(int32 n) { return Ut.heapString(n); }\n"
           "    public static void churn() {\n"
           "        int64 i = 0;\n"
           "        while (i < 400L) { String j #= Ut.heapString(64); i = i + 1; }\n"
           "    }\n"
           "    public static int32 work() {\n"
           "        " + workBody + "\n"
           "    }\n"
           "    public static int32 run() {\n"
           "        int64 base = Cajeta.liveCount();\n"
           "        int32 t = Ut.work();\n"
           "        int64 leaked = Cajeta.liveCount() - base;\n"
           "        return (int32) (leaked * 100) + t;\n"
           "    }\n"
           "}\n";
}

int32_t runJit(const std::string& workBody) {
    auto jit = CajetaJit::compile(makeSource(workBody), "test.Ut");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// The body of the first function whose name contains `needle`, or "" when absent.
std::string functionIr(const std::string& ir, const std::string& needle) {
    size_t pos = 0;
    while ((pos = ir.find("define ", pos)) != std::string::npos) {
        size_t eol = ir.find('\n', pos);
        if (ir.substr(pos, eol - pos).find(needle) != std::string::npos) {
            size_t end = ir.find("\n}\n", eol);
            return ir.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        }
        pos = eol;
    }
    return "";
}

}  // namespace

// 5.2.1: a `^String` kept with `=` is a borrow of every runtime form, and the holder never frees it.
TEST(StringFieldBorrowTests, borrowFormalStoreMakesNoCopyOrStake) {
    EXPECT_EQ(runJit(
        "String big #= Ut.heapString(300);\n"
        "String small #= Ut.heapString(40);\n"
        "String win #= big.substring(10, 40);\n"
        "{\n"
        "    K k = heap K();\n"
        "    int64 b0 = Cajeta.allocatedBytes();\n"
        "    int64 p0 = Cajeta.sharedPopulation();\n"
        "    k.keep(big);\n"
        "    k.keep(small);\n"
        "    k.keep(win);\n"
        "    if (Cajeta.allocatedBytes() != b0) { return -1; }\n"
        "    if (Cajeta.sharedPopulation() != p0) { return -2; }\n"
        "    if (k.v.size() != 30) { return -3; }\n"
        "}\n"
        "Ut.churn();\n"
        "if (big.size() != 300 || small.size() != 40) { return -4; }\n"
        "if (win.charAt(0) != (int8) 107) { return -5; }\n"
        "return 1;"), 1);
}

// 5.2.1: the store's IR resolves nothing.
TEST(StringFieldBorrowTests, borrowFormalStoreEmitsNoResolve) {
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(makeSource("return 1;"), "test.Ut", opts);
    std::string body = functionIr(jit->getModuleIr(), "test.K::keep(this:pointer,s:");
    ASSERT_FALSE(body.empty());
    EXPECT_EQ(body.find("__cajeta_string_resolve"), std::string::npos) << body;
}

// 5.2.2
TEST(StringFieldBorrowTests, literalStoreBorrowsTheStaticLiteral) {
    EXPECT_EQ(runJit(
        "K k = heap K();\n"
        "int64 b0 = Cajeta.allocatedBytes();\n"
        "k.v = \"a-static-literal-of-some-length\";\n"
        "if (Cajeta.allocatedBytes() != b0) { return -1; }\n"
        "return 1;"), 1);
}

// 5.2.3: a field read is an alias, and dropping the alias leaves the owner intact.
TEST(StringFieldBorrowTests, fieldReadStoreAliasesWithoutCopy) {
    EXPECT_EQ(runJit(
        "K o = heap K();\n"
        "o.keepSharp(#Ut.heapString(50));\n"
        "{\n"
        "    K k = heap K();\n"
        "    int64 b0 = Cajeta.allocatedBytes();\n"
        "    k.keepField(o);\n"
        "    if (Cajeta.allocatedBytes() != b0) { return -1; }\n"
        "}\n"
        "Ut.churn();\n"
        "if (o.v.size() != 50 || o.v.charAt(0) != (int8) 97) { return -2; }\n"
        "return 1;"), 1);
}

// 5.2.4: clone() is the copy that outlives its source.
TEST(StringFieldBorrowTests, cloneGivesAnIndependentCopy) {
    EXPECT_EQ(runJit(
        "K k = heap K();\n"
        "{\n"
        "    K o = heap K();\n"
        "    o.keepSharp(#Ut.heapString(600));\n"
        "    k.keepClone(o);\n"
        "}\n"
        "Ut.churn();\n"
        "if (k.v.size() != 600 || k.v.charAt(1) != (int8) 98) { return -1; }\n"
        "return 1;"), 1);
}

// 5.2.5: a `+` in place owns its result past the frame that built it.
TEST(StringFieldBorrowTests, concatInPlaceOwnsItsResult) {
    EXPECT_EQ(runJit(
        "K k = heap K();\n"
        "String a = \"abcdefghijklm\";\n"
        "String b = \"nopqrstuvwxyz\";\n"
        "k.keepConcat(a, b);\n"
        "Ut.churn();\n"
        "if (k.v.size() != 26 || k.v.charAt(13) != (int8) 110) { return -1; }\n"
        "return 1;"), 1);
}

// Rule 7: a plain call's flag rides the return, so an owned result is the field's and a view is a borrow.
TEST(StringFieldBorrowTests, plainCallInPlaceFollowsItsFlag) {
    EXPECT_EQ(runJit(
        "K k = heap K();\n"
        "k.keepCall();\n"
        "K w = heap K();\n"
        "int64 b0 = Cajeta.allocatedBytes();\n"
        "w.keepView(k);\n"
        "if (Cajeta.allocatedBytes() != b0) { return -1; }\n"
        "Ut.churn();\n"
        "if (k.v.size() != 40 || w.v.size() != 40) { return -2; }\n"
        "return 1;"), 1);
}

// A constructor store and a template field store follow the same rule.
TEST(StringFieldBorrowTests, constructorAndTemplateStoresBorrow) {
    EXPECT_EQ(runJit(
        "String s #= Ut.heapString(40);\n"
        "{\n"
        "    int64 b0 = Cajeta.allocatedBytes();\n"
        "    C c = heap C(s);\n"
        "    int64 afterCtor = Cajeta.allocatedBytes();\n"
        "    Box<String> bx = heap Box<String>();\n"
        "    int64 b1 = Cajeta.allocatedBytes();\n"
        "    bx.put(s);\n"
        "    if (Cajeta.allocatedBytes() != b1) { return -1; }\n"
        "    if (c.v.size() != 40) { return -2; }\n"
        "    if (afterCtor - b0 > 64) { return -3; }\n"
        "}\n"
        "Ut.churn();\n"
        "if (s.size() != 40 || s.charAt(0) != (int8) 97) { return -4; }\n"
        "return 1;"), 1);
}

// Card 5 twin: `#=` of a lent window still resolves, so the field outlives the window's owner.
TEST(StringFieldBorrowTests, sharpStoreOfLentWindowStillResolves) {
    EXPECT_EQ(runJit(
        "K k = heap K();\n"
        "{\n"
        "    String big #= Ut.heapString(300);\n"
        "    String win #= big.substring(10, 40);\n"
        "    k.keepSharp(win);\n"
        "}\n"
        "Ut.churn();\n"
        "if (k.v.size() != 30 || k.v.charAt(0) != (int8) 107) { return -1; }\n"
        "return 1;"), 1);
}
