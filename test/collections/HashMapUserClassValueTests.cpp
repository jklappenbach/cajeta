//
// title-tracking Unit 1 (plan 1.1) — the "HashMap<K, UserClass> crash"
// (found 2026-07-10, spec §8.3.2). Diagnosis: nothing to do with user-class
// values — `heap HashMap<...>()` had no matching constructor (only
// `HashMap(int64)` existed), and invokeMethod silently returned nullptr on
// unresolved ctors, leaving a memset-zero map (null `ctrl`) that SIGSEGV'd
// on first put. Every pre-existing test passed a capacity, so the silent
// skip was unexercised. Fixes covered here: HashMap no-arg ctor,
// no-matching-constructor hard error, and (prerequisite for the hard
// error) the one-arg Optional empty ctor replacing the never-resolving
// `Optional<T>(false, null)` idiom.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

const char* kMyObjectSrc =
    "package test;\n"
    "import cajeta.collection.HashMap;\n"
    "@AllArgsConstructor\n"
    "public class MyObject {\n"
    "    public int32 key;\n"
    "    public String value;\n"
    "}\n";

int32_t runI32(const std::string& src, const char* entryClass = "test.D") {
    auto jit = CajetaJit::compile(src, entryClass);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// Root cause of the original crash: `heap HashMap<...>()` had NO matching
// constructor (only `HashMap(int64)` existed) and invokeMethod silently
// returned nullptr — malloc + memset + vtable, no ctor, null `ctrl`, SIGSEGV
// on first put. Now: (a) HashMap has a no-arg ctor, and (b) an unresolved
// constructor is a hard compile error.
TEST(HashMapUserClassValueTests, unresolvedConstructorIsHardError) {
    std::string src = std::string(kMyObjectSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        MyObject bad = heap MyObject(\"wrong\", \"arity\", 3);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        ADD_FAILURE() << "expected CAJETA_ERROR_NO_MATCHING_CONSTRUCTOR";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_NO_MATCHING_CONSTRUCTOR");
        EXPECT_NE(e.getMessage().find("MyObject"), std::string::npos);
    } catch (const std::exception& e) {
        ADD_FAILURE() << "wrong exception type: " << e.what();
    }
}

// A class that declares NO constructors gets the synthesized default
// (ensureDefaultConstructor) — the hard error must not fire on its zero-arg
// construction. The template shape (base Stream<T>) regressed because the
// synthesized default was named with the instantiation's arg-suffixed
// typeName and never resolved; pinned by StreamTests.baseStreamCountIsZero.
TEST(HashMapUserClassValueTests, noCtorClassDefaultConstructs) {
    std::string src =
        "package test;\n"
        "public class Bare {\n"
        "    public int32 n;\n"
        "    public int32 get() { return this.n; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Bare b = heap Bare();\n"
        "        return b.get();\n"  // zero-init contract: 0
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// Args passed to a ctor-less class can never resolve — still a hard error.
TEST(HashMapUserClassValueTests, noCtorClassWithArgsIsHardError) {
    std::string src =
        "package test;\n"
        "public class Bare {\n"
        "    public int32 n;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Bare b = heap Bare(7);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        ADD_FAILURE() << "expected CAJETA_ERROR_NO_MATCHING_CONSTRUCTOR";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_NO_MATCHING_CONSTRUCTOR");
        EXPECT_NE(e.getMessage().find("Bare"), std::string::npos);
    } catch (const std::exception& e) {
        ADD_FAILURE() << "wrong exception type: " << e.what();
    }
}

// 1.1.1 — single put (plain, no transfer) + indexer read of a field.
TEST(HashMapUserClassValueTests, putAndIndexerReadOfClassValue) {
    std::string src = std::string(kMyObjectSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<int32, MyObject> map = heap HashMap<int32, MyObject>();\n"
        "        MyObject obj = heap MyObject(3, \"hello\");\n"
        "        map.put(obj.key, obj);\n"
        "        MyObject picked = map[3];\n"
        "        return picked.key;\n"  // 3
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

// 1.1.2 — same access through a wrapper: chained call + indexer.
TEST(HashMapUserClassValueTests, chainedGetterIndexerRead) {
    std::string src = std::string(kMyObjectSrc) +
        "public class Holder {\n"
        "    public HashMap<int32, MyObject> map;\n"
        "    public Holder() { this.map = heap HashMap<int32, MyObject>(); }\n"
        "    public HashMap<int32, MyObject> getMap() { return this.map; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder holder = heap Holder();\n"
        "        MyObject obj = heap MyObject(7, \"chained\");\n"
        "        holder.getMap().put(obj.key, obj);\n"
        "        return holder.getMap()[7].key;\n"  // 7
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// 1.1.3 — loop of 10 puts + full teardown: no crash, allocations accounted.
TEST(HashMapUserClassValueTests, loopedPutsTeardownAccounted) {
    std::string src = std::string(kMyObjectSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        HashMap<int32, MyObject> map = heap HashMap<int32, MyObject>();\n"
        "        int32 last = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 10) {\n"
        "            MyObject obj = heap MyObject(i, \"v\" + i);\n"
        "            map.put(obj.key, obj);\n"
        // Read while the element is alive: plain put is a borrow store
        // (the loop local still owns; it drops at iteration exit).
        "            last = map[i].key;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return last;\n"  // 9
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        // Borrow-stored elements are owned by the loop locals whose drop
        // entries fire at loop-body scope exit; the map borrows. Expect
        // no NEGATIVE delta (double free) and no crash. Positive delta
        // would be a leak; pin the exact accounting once fixed.
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 9);
}
