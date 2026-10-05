// element-ownership Unit 3 — store-site resolution gate (spec §7.1.3, §6.1.1;
// plan 3.1.6 / 3.2.1). The String field-store resolution must be GATED on
// statically-known SOURCE ownership: a bare identifier bound to a `#`-transfer
// parameter is an OWNED source, so `this.field = param` must MOVE the wrapper
// into the field — not materialize a fresh copy that strands the moved-in
// source (leak 15.12.1).
//
// We isolate the gate from field TEARDOWN (which lands with the drop-walk in a
// later sub-unit) using a DIFFERENTIAL: two holders differing only in the
// field-store shape —
//   (control) `this.s #= s;`  — an explicit `#`-move, already a transfer, and
//   (test)    `this.s = s;`   — a bare `#`-param store.
// Both leave their String field undropped, so any field-teardown leak is
// COMMON to both. The only population difference is the copy-strand: before the
// gate the bare store copies, stranding one heap buffer per iteration, so its
// live-count delta exceeds the move baseline by ~n. After the gate the bare
// store moves and the two deltas are equal.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

// A Holder whose ctor stores its `#String` parameter into the `s` field using
// `storeStmt`, and a driver that builds n holders from freshly-owned (heap,
// non-SSO) concat Strings and reports the live-count delta across the build.
std::string src(const char* storeStmt) {
    return std::string(
        "package test;\n"
        "public final class Holder {\n"
        "    public String s;\n"
        "    public Holder(#String s) { ") + storeStmt + " }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 build(int32 n) {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 i = 0; int64 acc = 0;\n"
        "        while (i < n) {\n"
        // 40-char prefix forces a real heap buffer (past SSO), so a resolve
        // COPY strands an observable allocation; a move does not.
        "            String k = \"key-prefix-long-enough-to-exceed-sso-\" + i;\n"
        "            Holder h = heap Holder(#k);\n"
        "            acc = acc + (int64) h.s.size();\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (acc < (int64) 0) { return (int64) -1; }\n"
        "        return Cajeta.liveCount() - base;\n"
        "    }\n"
        "}\n";
}

int64_t deltaFor(const char* storeStmt, int32_t n) {
    auto jit = CajetaJit::compile(src(storeStmt), "test.D");
    auto fn = jit->lookup<int64_t (*)(int32_t)>("build");
    return fn(n);
}

}  // namespace

// field-store-ownership 3.2.4: a bare `=` of a `#`-param is rejected, and the `#=` store
// takes the title, so the loop strands no buffer.
TEST(OwningStoreGateTests, bareHashParamStoreRejectedAndSharpStoreMoves) {
    std::string code;
    try {
        CajetaJit::compile(src("this.s = s;"), "test.D");
    } catch (cajeta::Exception& e) {
        code = e.getErrorId();
    }
    EXPECT_EQ(code, "CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE");
    EXPECT_EQ(deltaFor("this.s #= s;", 2000), 0);
}
