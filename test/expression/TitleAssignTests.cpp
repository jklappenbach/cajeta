// title-stores Unit 1 (spec §2.1, §2.2, §2.5.2): the `#=` title-assign
// operator. RED until 1.2.x lands the token + lowering.
//
// Parity contract: `dst #= v` is byte-for-byte today's `dst #= v` at each
// valid destination (field, local decl-init, array slot, indexed user
// class). Oracles are the SignatureAbi liveCount pattern: an owned RHS
// moves (destination drops it, source entry consumed), a borrowed RHS
// forwards borrow (source's owner keeps the single drop).

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kCellSrc =
    "package test;\n"
    "public class Cell {\n"
    "    public int32 n;\n"
    "    public Cell(int32 nn) { this.n = nn; }\n"
    "}\n";

int32_t runI32(const std::string& src, const char* entryClass = "test.D") {
    auto jit = CajetaJit::compile(src, entryClass);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// 1.1.1a — field destination: `this.c #= v` forwards the caller's actual
// title exactly like `this.c #= v` (owned put → holder drops it at
// teardown; lent put → source survives the holder).

// 1.1.1b — local declaration-initializer: `Cell x #= mk()` arms/forwards
// exactly like `Cell x #= mk()` (spec 2.2.3): fresh result owned and
// dropped at scope exit, zero leak.

// 1.1.1c — raw array slot: `data[i] #= v` matches `data[i] #= v` (today's
// local-owning-array move semantics; per-slot bits arrive in Unit 3).

// 1.1.1d — indexed user class: `m[k] #= v` lowers through operator[]=
// with the transfer word composed, same as `m[k] #= v` (spec 2.2.4).

// 1.1.2 — `operator#=` is not declarable (spec §5.5): ownership-store
// semantics are compiler-owned, like heap/stack placement.
TEST(TitleAssignTests, operatorSharpAssignNotDeclarable) {
    std::string src = std::string(kCellSrc) +
        "public class Box {\n"
        "    public Cell c;\n"
        "    public Box() { this.c = null; }\n"
        "    public void operator#=(Cell v) { this.c #= v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    // Accept either a dedicated diagnostic or a parse rejection — the pin
    // is that it must NOT compile.
    EXPECT_ANY_THROW({
        auto jit = CajetaJit::compile(src, "test.D");
        (void) jit;
    });
}

// 1.1.3 — non-assignment `#` positions unaffected by the new token: call
// arg, return, and guarded field extraction all keep today's behavior in
// a source that ALSO uses `#=` (longest-match must not eat `#` elsewhere).
TEST(TitleAssignTests, sharpElsewhereUnaffected) {
    std::string src = std::string(kCellSrc) +
        "public class Keep {\n"
        "    public Cell c;\n"
        "    public Keep() { this.c = null; }\n"
        "    public void put(#Cell v) { this.c #= v; }\n"
        "    public #Cell take() {\n"
        "        if (this.c == null) { return null; }\n"
        "        Cell m #= this.c;\n"
        "        this.c = null;\n"
        "        return #m;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static #Cell fresh() { return heap Cell(8); }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Keep k = heap Keep();\n"
        "            k.put(#fresh());\n"
        "            Cell got #= k.take();\n"
        "            t = got.n;\n"
        "        }\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 8);
}
