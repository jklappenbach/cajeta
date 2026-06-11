//
// Phase 3a of #68 (docs/stdlib/OwnershipTransfer.md): body-side
// borrow-escape checks. When a method's formal parameter is declared
// plain `T` (class-typed), it's a borrow — the caller still owns the
// value and the callee can use it within the call but can't escape it
// past the call boundary.
//
// Two escape shapes land in 3a:
//
//   (1) Returning a borrowed parameter through a `#T`-return signature
//       — the caller would register a fresh drop entry expecting
//       ownership, freeing the borrowed value out from under the
//       original owner.
//
//   (2) Re-transferring a borrowed parameter via `#x` to another call
//       — the caller is claiming to surrender ownership of a value
//       they don't own.
//
// Two other shapes land in a follow-up (3b/3c): field-store of borrow,
// and capture of borrow in a returned closure.
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

} // namespace

// (1) Return shape: `#T` return + plain-T borrow param + `return param;`.
// Rejected: callee promises ownership transfer but returns a borrow it
// doesn't own.
TEST(BorrowParamEscapeTests, returnBorrowParamThroughOwnershipSignatureRejected) {
    auto src =
        "package test;\n"
        "public class Foo {\n"
        "    public int32 v;\n"
        "    public Foo(int32 v) { this.v = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static #Foo escape(Foo f) {\n"  // borrow formal, # return
        "        return f;\n"                       // escape — rejected
        "    }\n"
        "    public static int32 run() {\n"
        "        Foo a = heap Foo(7);\n"
        "        Foo b = escape(a);\n"
        "        return b.v;\n"
        "    }\n"
        "}\n";
    EXPECT_THROW(runI32(src), cajeta::Exception);
}

// Control: `#T` return + `#T` formal + `return p;` works. The caller
// surrendered ownership at the outer call; the callee owns p and passes
// it through.
TEST(BorrowParamEscapeTests, returnTransferredParamThroughOwnershipSignatureAccepted) {
    auto src =
        "package test;\n"
        "public class Foo {\n"
        "    public int32 v;\n"
        "    public Foo(int32 v) { this.v = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static #Foo passThrough(#Foo f) {\n"
        "        return f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Foo a = heap Foo(11);\n"
        "        Foo b = passThrough(#a);\n"
        "        return b.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}

// Control: plain-`T` return + plain-`T` formal + `return p;` is the
// borrow-pass-through pattern. Both ends know it's a borrow.
TEST(BorrowParamEscapeTests, returnBorrowParamThroughBorrowSignatureAccepted) {
    auto src =
        "package test;\n"
        "public class Foo {\n"
        "    public int32 v;\n"
        "    public Foo(int32 v) { this.v = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static Foo identity(Foo f) {\n"  // plain return + plain formal
        "        return f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Foo a = heap Foo(13);\n"
        "        Foo b = identity(a);\n"  // borrow passthrough
        "        return b.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 13);
}

// (2) `#`-transfer shape: caller writes `#p` where p is a borrow param.
// Rejected: caller is claiming to transfer something they don't own.
TEST(BorrowParamEscapeTests, transferBorrowParamViaSharpRejected) {
    auto src =
        "package test;\n"
        "public class Foo {\n"
        "    public int32 v;\n"
        "    public Foo(int32 v) { this.v = v; }\n"
        "}\n"
        "public class Sink {\n"
        "    public Foo f;\n"
        "    public Sink(#Foo f) { this.f = f; }\n"  // requires #
        "}\n"
        "public final class D {\n"
        "    public static int32 useBorrow(Foo p) {\n"  // borrow formal
        "        Sink s = heap Sink(#p);\n"             // escape via #p — rejected
        "        return s.f.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Foo a = heap Foo(17);\n"
        "        return useBorrow(a);\n"
        "    }\n"
        "}\n";
    EXPECT_THROW(runI32(src), cajeta::Exception);
}

// Control: when the outer param IS `#T`, transferring it onward via `#p`
// is the legitimate pass-through-with-ownership shape.
TEST(BorrowParamEscapeTests, transferTransferredParamViaSharpAccepted) {
    auto src =
        "package test;\n"
        "public class Foo {\n"
        "    public int32 v;\n"
        "    public Foo(int32 v) { this.v = v; }\n"
        "}\n"
        "public class Sink {\n"
        "    public Foo f;\n"
        "    public Sink(#Foo f) { this.f = f; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 hand(#Foo p) {\n"  // outer #Foo formal
        "        Sink s = heap Sink(#p);\n"          // legitimate transfer
        "        return s.f.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Foo a = heap Foo(23);\n"
        "        return hand(#a);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 23);
}
