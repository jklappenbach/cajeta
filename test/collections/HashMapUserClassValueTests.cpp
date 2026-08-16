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

// Args passed to a ctor-less class can never resolve — still a hard error.

// 1.1.1 — single put + indexer read of a field. uniform-transfer 2.3: the
// map OWNS its values, so the put surrenders and `obj` stays readable as a
// demoted borrow.
TEST(HashMapUserClassValueTests, putAndIndexerReadOfClassValue) {
    std::string src = std::string(kMyObjectSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<int32, MyObject> map = heap HashMap<int32, MyObject>();\n"
        "        MyObject obj = heap MyObject(3, \"hello\");\n"
        "        map.put(obj.key, #obj);\n"
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
        "        holder.getMap().put(obj.key, #obj);\n"
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
        "            map.put(obj.key, #obj);\n"
        // Read through the map while the element is alive. The MAP owns it
        // now (uniform-transfer 2.3), so it survives the loop iteration
        // that created it and is reclaimed at the map's teardown.
        "            last = map[i].key;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return last;\n"  // 9
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        // The map owns all 10 elements and drops them at its teardown, so
        // the scope must balance exactly: any non-zero delta is a leak
        // (positive) or a double free (negative), and either shows up as a
        // wrong return value rather than a silent pass.
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 9);
}
