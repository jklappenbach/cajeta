//
// VEA-3b — view element arrays: offset table, descriptor, O(1) pins.
//
// The ctor's validation sweep sizes and fills a frame-arena-backed offset
// table; the view value becomes an arena {i8* data, i64* table} descriptor.
// Access reads offsets from the table: element access is O(1) — no
// per-access walk over preceding elements.
//
// IR probe naming contract (kept distinct on purpose):
//   - ctor validation loop blocks:   earr_hdr / earr_body / earr_exit
//   - ctor table-fill loop blocks:   vea_fill_hdr / ...
//   - ACCESS-side walk loop blocks:  earr_walk_hdr / ...  (the fallback the
//     table makes unreachable — its absence IS the O(1) pin)
//   - table-based access values:     earr_tbl_start/region/elem/stride
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.T");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

CajetaJit::Options irOpts() {
    CajetaJit::Options o;
    o.captureIr = true;
    return o;
}

const char* kViews =
    "package test;\n"
    "@HostEndian\n"
    "public view D {\n"
    "    int32  s;\n"
    "    String name;\n"
    "}\n"
    "@HostEndian\n"
    "public view M {\n"
    "    int32 magic;\n"
    "    D[]   ds;\n"
    "}\n";

} // namespace

// --- O(1) pin: element access reads the table, never walks -----------------

TEST(ViewElementArrayTableTests, elementAccessIsTableBasedNotWalk) {
    auto src = std::string(kViews) +
        "public final class T {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[8];\n"
        "        bytes[0] = 7;\n"
        "        bytes[1] = 2;\n"
        "        bytes[2] = 5;\n"
        "        bytes[3] = 4;\n"
        "        bytes[4] = 1684234849;\n"
        "        bytes[5] = 9;\n"
        "        bytes[6] = 4;\n"
        "        bytes[7] = 2054781047;\n"
        "        M m = M(bytes);\n"
        "        return m.ds[1].s;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T", irOpts());
    const std::string& ir = jit->getModuleIr();
    // Table-based element offset load present...
    EXPECT_NE(ir.find("earr_tbl_"), std::string::npos);
    // ...and no access-site element walk was emitted anywhere.
    EXPECT_EQ(ir.find("earr_walk_hdr"), std::string::npos);
    // Behavior unchanged.
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 9);
}

// --- fixed-stride elements: no per-element table region --------------------

TEST(ViewElementArrayTableTests, fixedStrideElementsHaveNoFillLoop) {
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Pt { int32 x; int32 y; }\n"
        "@HostEndian\n"
        "public view Poly {\n"
        "    int32 kind;\n"
        "    Pt[]  points;\n"
        "}\n"
        "public final class T {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[6];\n"
        "        bytes[0] = 1;\n"
        "        bytes[1] = 2;\n"
        "        bytes[2] = 10;\n"
        "        bytes[3] = 20;\n"
        "        bytes[4] = 30;\n"
        "        bytes[5] = 40;\n"
        "        Poly p = Poly(bytes);\n"
        "        return p.points[1].y;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T", irOpts());
    const std::string& ir = jit->getModuleIr();
    // No per-element fill loop (the table has no region for fixed-stride
    // elements) and no access walk; stride math off the table's start slot.
    EXPECT_EQ(ir.find("vea_fill_hdr"), std::string::npos);
    EXPECT_EQ(ir.find("earr_walk_hdr"), std::string::npos);
    EXPECT_NE(ir.find("earr_tbl_stride"), std::string::npos);
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 40);
}

// --- owning form rejected for descriptor views -----------------------------

TEST(ViewElementArrayTableTests, owningFormRejected) {
    auto src = std::string(kViews) +
        "public final class T {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[2];\n"
        "        bytes[0] = 7;\n"
        "        bytes[1] = 0;\n"
        "        M m = M(#bytes);\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.T");
        FAIL() << "expected CAJETA_ERROR_VIEW_ELEMENT_ARRAY_OWNING";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VIEW_ELEMENT_ARRAY_OWNING");
    }
}

// --- arena reclaim: constructing in a loop doesn't accumulate --------------

// Table + descriptor live in the per-fiber frame arena; the loop body's
// scope reset reclaims them each iteration. 10k constructions would OOM the
// 4 GiB arena reserve only if per-iteration allocations never reclaimed and
// were large — this is a liveness smoke that the mark/reset pairing holds
// (a broken pairing aborts under drop-chain accounting in debug runtimes).
TEST(ViewElementArrayTableTests, loopConstructionReclaimsArena) {
    auto src = std::string(kViews) +
        "public final class T {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[8];\n"
        "        bytes[0] = 7;\n"
        "        bytes[1] = 2;\n"
        "        bytes[2] = 5;\n"
        "        bytes[3] = 4;\n"
        "        bytes[4] = 1684234849;\n"
        "        bytes[5] = 9;\n"
        "        bytes[6] = 4;\n"
        "        bytes[7] = 2054781047;\n"
        "        int32 acc = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 10000) {\n"
        "            M m = M(bytes);\n"
        "            acc = acc + m.ds[1].s;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (acc != 90000) return 10;\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
