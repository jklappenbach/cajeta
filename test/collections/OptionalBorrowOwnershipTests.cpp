// optional-borrow-ownership Unit 2 — `Optional<T>`'s `#T` ctor parameter must be
// MODE-DEPENDENT. In a plain (borrow-mode) `Optional<T>` instantiation it must
// accept a borrow and must not drop the payload; in `Optional<#T>` it must still
// demand the transfer and still drop.
//
// Before this: element-ownership Unit 3B wired the owned-element drop walk for
// `P[]` fields only ("scalar `P`-typed fields already ride the class-ref drop
// branch"). Optional's field is a scalar `T value`, so it rode that mode-unaware
// branch and freed a borrowed payload — the use-after-free behind
// Throwable.toJson()'s crash on a 2-link cause chain.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>

using cajeta_test::CajetaJit;

namespace {
std::unique_ptr<CajetaJit> jitOf(const char* body) {
    std::string src =
        std::string("package test;\n") +
        "import cajeta.error.Exception;\n" +
        "import cajeta.lang.Optional;\n" +
        "public final class D {\n" + body + "}\n";
    return CajetaJit::compile(src, "test.D");
}
}

// 2.1.2 — a borrowing Optional dropped each loop iteration must NOT free the
// field it wraps. Oracle: spec 1.4.1 probe A. One cause link is enough; the
// second iteration re-reads a field the first iteration's Optional would have
// freed. SIGSEGV before the fix.
TEST(OptionalBorrowOwnership, borrowedOptionalDropDoesNotFreePayload) {
    auto jit = jitOf(
        "    public static int32 run() {\n"
        "        try {\n"
        "            throw heap Exception(\"L1\", heap Exception(\"L2\"));\n"
        "        } catch (Exception e) {\n"
        "            int32 n = 0;\n"
        "            int32 i = 0;\n"
        "            while (i < 2) {\n"
        "                Optional<Throwable> tmp = e.getCause();\n"
        "                n = n + tmp.get().getMessage().count();\n"
        "                i = i + 1;\n"
        "            }\n"
        "            return n;\n"
        "        }\n"
        "        return -1;\n"
        "    }\n");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 4) << "two reads of \"L2\" (count 2 each); a freed cause reads garbage";
}

// 2.1.3 — the checker half. Constructing a plain `Optional<T>` from a BORROWED
// local must compile. Before this: CAJETA_ERROR_TRANSFER_REQUIRED (probe D), so
// no user code could express the borrowing Optional `getCause()` needs.
TEST(OptionalBorrowOwnership, plainOptionalAcceptsBorrowedArgument) {
    auto jit = jitOf(
        "    public static int32 run() {\n"
        "        Exception ex = heap Exception(\"borrowed\");\n"
        "        Optional<Throwable> o = stack Optional<Throwable>(true, ex);\n"
        "        return o.get().getMessage().count();\n"
        "    }\n");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 8) << "\"borrowed\".count() == 8";
}

// 2.1.1 — the owned mode is untouched: `Optional<#T>` still takes ownership and
// still drops at scope exit. Regression guard on the fix for 2.1.2, using the
// same liveCount oracle as OwnershipLeakProbe.
TEST(OptionalBorrowOwnership, ownedOptionalStillDropsPayload) {
    auto jit = jitOf(
        "    public static void spin(int32 n) {\n"
        "        int32 j = 0;\n"
        "        while (j < n) {\n"
        "            Optional<Throwable> o = stack Optional<Throwable>(true, #heap Exception(\"x\"));\n"
        "            j = j + 1;\n"
        "        }\n"
        "    }\n"
        "    public static int64 run(int32 n) {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        spin(n);\n"
        "        return Cajeta.liveCount() - base;\n"
        "    }\n");
    auto fn = jit->lookup<int64_t (*)(int32_t)>("run");
    EXPECT_LT(fn(2000), 50) << "owned Optional<#Throwable> leaked its payload at scope exit";
}

// 2.1.5 — primitives carry no ownership; unchanged by all of the above.
TEST(OptionalBorrowOwnership, primitiveOptionalUnchanged) {
    auto jit = jitOf(
        "    public static int32 run() {\n"
        "        Optional<int32> some = stack Optional<int32>(true, 42);\n"
        "        Optional<int32> none = stack Optional<int32>(false, 0);\n"
        "        return some.get() + none.orElse(-1);\n"
        "    }\n");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 41) << "42 + (-1)";
}
