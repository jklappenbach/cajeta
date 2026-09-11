// `#=` forwards a borrow; `#v` demands a title.
//
// The two spellings are not interchangeable, and the difference is what the
// source is REQUIRED to hold:
//
//   dst = #src   a transfer. `#src` asserts the source holds a title to
//                surrender, so a titleless source is CAJETA_ERROR_MOVE_OF_BORROW.
//   dst #= src   a passthrough. It asserts nothing and claims nothing: from an
//                owner it transfers, from a borrow it forwards the borrow. No
//                second owner is minted, so nothing can double-free.
//
// "Titleless" covers both shapes — a name that NEVER owned (an alias, a field
// read, a plain call's result) and one DEMOTED by an earlier transfer. Neither
// has a title, so `#=` borrows from either.
//
// Regression history: `#=` used to reject a demoted source (Scope.cpp check
// (a)), and the DECLARATION form `T c #= b` additionally rejected a never-owned
// borrow that the assignment form `c #= b` accepted — the declaration's node
// reaches the check through HeapField::getOrCreateAllocation without the
// mode-carrying flag, so the check now keys on isSharpStore().

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

std::string wrap(const std::string& body) {
    return std::string("package test;\n")
        + "public final class D {\n"
        + "    public static int32 run() {\n"
        + "        int8[] a = heap int8[4];\n"
        + "        a[0] = (int8) 7;\n"
        + body
        + "    }\n"
        + "}\n";
}

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

void compileExpectError(const std::string& src, const std::string& expectCode) {
    try {
        CajetaJit::compile(src, "test.D");
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), expectCode);
        return;
    } catch (const std::exception& e) {
        ADD_FAILURE() << "expected " << expectCode << ", got " << e.what();
        return;
    }
    ADD_FAILURE() << "expected a compile error, got a clean compile";
}

}  // namespace

// --- `#=` forwards, and never rejects -------------------------------------

// A DEMOTED source: `a` handed its title to `b`, so the second store forwards
// the borrow `a` now holds. `b` remains the single owner and drops once.
TEST(PassthroughFromBorrowTests, sharpStoreFromDemotedSourceBorrows) {
    EXPECT_EQ(runI32(wrap(
        "        int8[] b #= a;\n"
        "        int8[] c #= a;\n"
        "        return (int32) c[0];\n")), 7);
}

// A NEVER-OWNED source, in the DECLARATION form — the case the declaration
// path used to reject while the assignment form accepted it.
TEST(PassthroughFromBorrowTests, sharpStoreDeclarationFromAliasBorrows) {
    EXPECT_EQ(runI32(wrap(
        "        int8[] b = a;\n"
        "        int8[] c #= b;\n"
        "        return (int32) c[0];\n")), 7);
}

// The same store as an ASSIGNMENT. Declaration and assignment must agree.
TEST(PassthroughFromBorrowTests, sharpStoreAssignmentFromAliasBorrows) {
    EXPECT_EQ(runI32(wrap(
        "        int8[] b = a;\n"
        "        int8[] c = heap int8[1];\n"
        "        c #= b;\n"
        "        return (int32) c[0];\n")), 7);
}

// A field destination forwards the borrow the same way.
TEST(PassthroughFromBorrowTests, sharpStoreIntoFieldFromAliasBorrows) {
    const char* src =
        "package test;\n"
        "public class Box { public int8[] v; public Box() { this.v = heap int8[1]; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] a = heap int8[4];\n"
        "        a[0] = (int8) 7;\n"
        "        int8[] b = a;\n"
        "        Box k = heap Box();\n"
        "        k.v #= b;\n"
        "        return (int32) k.v[0];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// --- `#v` still demands a title -------------------------------------------

// Transferring twice from the same owner: the second `#a` asks for a title
// that already left. This is the error `#=` is NOT.
TEST(PassthroughFromBorrowTests, transferTwiceFromOwnerRejected) {
    compileExpectError(wrap(
        "        int8[] b = #a;\n"
        "        int8[] c = #a;\n"
        "        return (int32) c[0];\n"),
        "CAJETA_ERROR_MOVE_OF_BORROW");
}

// Transferring out of a never-owned alias: same demand, same rejection.
TEST(PassthroughFromBorrowTests, transferFromAliasBorrowRejected) {
    compileExpectError(wrap(
        "        int8[] b = a;\n"
        "        int8[] c = #b;\n"
        "        return (int32) c[0];\n"),
        "CAJETA_ERROR_MOVE_OF_BORROW");
}
