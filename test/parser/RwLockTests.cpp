//
// cajeta.threading.RwLock<T> — read-heavy shared state (R7-D). Many readers
// share the read lock; a writer holds it exclusively (writer-preference in
// the runtime). Closure/scoped shape like Mutex<T>: read() snapshots under a
// shared lock; withWrite((T)->T) mutates under the exclusive lock.
//
// Loaded from the embedded standard library
// (runtime/src/cajeta/threading/{RwLock,WriteGuard}.cajeta).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string rwTestSource(const std::string& dBody) {
    return std::string("package test;\n")
        + "import cajeta.threading.RwLock;\n"
        + "public final class D {\n" + dBody + "}\n";
}

int32_t runI32(const std::string& dBody) {
    auto jit = CajetaJit::compile(rwTestSource(dBody), "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

TEST(RwLockTests, constructAndDestroy) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        RwLock<int32> rw = new RwLock<int32>(7);\n"
        "        return 1;\n"
        "    }\n"
    ), 1);
}

TEST(RwLockTests, readReturnsInitial) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        RwLock<int32> rw = new RwLock<int32>(42);\n"
        "        return rw.read();\n"
        "    }\n"
    ), 42);
}

TEST(RwLockTests, withWriteMutatesThenRead) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        RwLock<int32> rw = new RwLock<int32>(0);\n"
        "        rw.withWrite((int32 v) -> v + 9);\n"
        "        return rw.read();\n"
        "    }\n"
    ), 9);
}

TEST(RwLockTests, repeatedWritesAccumulate) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        RwLock<int32> rw = new RwLock<int32>(0);\n"
        "        rw.withWrite((int32 v) -> v + 1);\n"
        "        rw.withWrite((int32 v) -> v + 2);\n"
        "        rw.withWrite((int32 v) -> v + 4);\n"
        "        return rw.read();\n"
        "    }\n"
    ), 7);
}

// A writer fiber acquires the exclusive write lock, mutates, releases; the
// scope joins before run() reads the result. Exercises the fiber wrlock path.
TEST(RwLockTests, fiberWriter) {
    EXPECT_EQ(runI32(
        "    public static async int32 writer(RwLock<int32> rw) {\n"
        "        rw.withWrite((int32 v) -> 100);\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        RwLock<int32> rw = new RwLock<int32>(0);\n"
        "        scope {\n"
        "            spawn writer(rw);\n"
        "        }\n"
        "        return rw.read();\n"
        "    }\n"
    ), 100);
}

// Two reader fibers both take the shared read lock and read the value into
// a shared output array; the scope joins before summing. Exercises the
// fiber rdlock path with concurrent readers (neither excludes the other).
TEST(RwLockTests, concurrentReaders) {
    EXPECT_EQ(runI32(
        "    public static async int32 readInto(RwLock<int32> rw, int32[] out, int32 idx) {\n"
        "        out[idx] = rw.read();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        RwLock<int32> rw = new RwLock<int32>(5);\n"
        "        int32[] outArr = new int32[2];\n"
        "        scope {\n"
        "            spawn readInto(rw, outArr, 0);\n"
        "            spawn readInto(rw, outArr, 1);\n"
        "        }\n"
        "        return outArr[0] + outArr[1];\n"
        "    }\n"
    ), 10);
}
