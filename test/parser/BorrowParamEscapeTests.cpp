//
// Phase 3a of #68 (docs/specification/lang/OwnershipTransfer.md): body-side
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
// title-tracking rev 2 respell (plan 5.2.4 / 7.2.1): for CLASS-typed
// formals the static rejections above are RETIRED — a plain formal is a
// runtime owner whose title is the caller's flag, so `return f` through a
// `#`-return and `#p` forwarding both just forward that flag (a lend
// forwards 0; no double free, the caller keeps ownership). The two
// "Rejected" pins below now pin the runtime pass-through behavior instead.
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

// (1) Return shape: `#T` return + plain-T formal + `return param;`.
// rev 2: the formal's runtime flag forwards through the return — a lent
// argument comes back still owned by the caller; no throw, no free.
TEST(BorrowParamEscapeTests, returnBorrowParamThroughOwnershipSignatureForwardsFlag) {
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
    EXPECT_EQ(runI32(src), 7);
}

// Control: `#T` return + `#T` formal + `return p;` works. The caller
// surrendered ownership at the outer call; the callee owns p and passes
// it through.

// Control: plain-`T` return + plain-`T` formal + `return p;` is the
// borrow-pass-through pattern. Both ends know it's a borrow.

// (2) `#`-transfer shape: caller writes `#p` where p is a plain formal.
// rev 2: `#p` forwards the formal's runtime flag — a lend forwards 0, so
// Sink stores a borrow and the original owner frees exactly once.
TEST(BorrowParamEscapeTests, transferBorrowParamViaSharpForwardsFlag) {
    auto src =
        "package test;\n"
        "public class Foo {\n"
        "    public int32 v;\n"
        "    public Foo(int32 v) { this.v = v; }\n"
        "}\n"
        "public class Sink {\n"
        "    public Foo f;\n"
        "    public Sink(#Foo f) { this.f #= f; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 useBorrow(Foo p) {\n"  // plain formal (lend)
        "        Sink s = heap Sink(#p);\n"             // forwards flag 0
        "        return s.f.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Foo a = heap Foo(17);\n"
        "        return useBorrow(a);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 17);
}

// Control: when the outer param IS `#T`, transferring it onward via `#p`
// is the legitimate pass-through-with-ownership shape.
