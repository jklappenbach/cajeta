//
// owned-return-of-borrowed-this — defect fix pins (spec §5.1).
//
// `return this;` from a `#T`-returning instance method used to COMPILE and
// hand the caller a fresh OWNED reference to the receiver's object. The
// caller's temp then legitimately drops it — freeing the wrapper out from
// under the receiver local (UAF, detonating a JIT session later when the
// block is reused). The receiver is a plain-borrow formal: the method holds
// no title to transfer, so the compiler now rejects the shape with
// CAJETA_ERROR_OWNED_RETURN_OF_BORROWED_THIS (matching the
// field-store-title-trap doctrine that plain formals never inherit
// ownership). Library fix for the no-change-fast-path idiom is a zero-copy
// borrow window (Cajeta.stringSliceBorrow — see String.replace / nfc).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

const char* kCellSrc =
    "package test;\n"
    "public class Cell {\n"
    "    public int32 n;\n"
    "    public Cell(int32 nn) { this.n = nn; }\n"
    "}\n";

std::string compileExpectError(const std::string& src,
                               const std::string& expectCode) {
    try {
        CajetaJit::compile(src, "test.D");
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), expectCode);
        return e.getMessage();
    } catch (const std::exception& e) {
        return e.what();
    }
    ADD_FAILURE() << "expected a compile error";
    return "";
}

int32_t runI32(const std::string& src, const char* entryClass = "test.D") {
    auto jit = CajetaJit::compile(src, entryClass);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// §5.1 — the direct shape: `return this` under a `#` return is rejected.
TEST(OwnedReturnOfBorrowedThisTests, returnThisUnderTransferRejected) {
    std::string src = std::string(kCellSrc) +
        "public class Box {\n"
        "    public int32 n;\n"
        "    public Box(int32 nn) { this.n = nn; }\n"
        "    public #Box same() {\n"
        "        return this;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    std::string msg = compileExpectError(
        src, "CAJETA_ERROR_OWNED_RETURN_OF_BORROWED_THIS");
    // The diagnostic must name the method and point at the fixes.
    EXPECT_NE(msg.find("same"), std::string::npos) << msg;
    EXPECT_NE(msg.find("borrow"), std::string::npos) << msg;
}

// §5.1 — the fast-path shape that bit String.nfc: one arm returns `this`,
// the other a fresh owned value. Still rejected — the `this` arm is the bug.
TEST(OwnedReturnOfBorrowedThisTests, fastPathReturnThisRejected) {
    std::string src = std::string(kCellSrc) +
        "public class Box {\n"
        "    public int32 n;\n"
        "    public Box(int32 nn) { this.n = nn; }\n"
        "    public #Box normalized() {\n"
        "        if (this.n == 0) {\n"
        "            return this;\n"
        "        }\n"
        "        return #heap Box(0);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_OWNED_RETURN_OF_BORROWED_THIS");
}

// Control — the fluent-builder shape (`return this` under a PLAIN return
// type) is a borrow ride-out and must keep compiling and running.
TEST(OwnedReturnOfBorrowedThisTests, plainReturnThisStillCompiles) {
    std::string src = std::string(kCellSrc) +
        "public class Builder {\n"
        "    public int32 n;\n"
        "    public Builder() { this.n = 0; }\n"
        "    public Builder add(int32 v) {\n"
        "        this.n = this.n + v;\n"
        "        return this;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Builder b = heap Builder();\n"
        "        return b.add(2).add(3).n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// Control — a `#` method returning a genuinely fresh owned value is the
// legitimate shape and must keep compiling and running leak-free.
TEST(OwnedReturnOfBorrowedThisTests, freshOwnedReturnStillCompiles) {
    std::string src = std::string(kCellSrc) +
        "public class Box {\n"
        "    public int32 n;\n"
        "    public Box(int32 nn) { this.n = nn; }\n"
        "    public #Box doubled() {\n"
        "        return #heap Box(this.n * 2);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Box b = heap Box(4);\n"
        "            Box d #= b.doubled();\n"
        "            t = d.n;\n"
        "        }\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 8);
}
