//
// Cross-file forward references to VIEWS (the gossip Delta[]-before-Delta
// bug, 2026-07-20). File visit order is directory/map order; when a
// consumer parses BEFORE the view it references, CajetaType::fromContext
// used to synthesize a plain CajetaClass placeholder — view classification
// (CajetaView::isElementArray, descriptor-view detection) and member
// lookup then bound to a propertyless class shell and failed with
// MEMBER_NOT_FOUND (or silently declassified the element array).
//
// The fix mirrors enums/interfaces: the prescan marks view declarations
// (markArchiveView), fromContext synthesizes a CajetaVIEW placeholder,
// and visitViewDeclaration fills the SAME shared_ptr (placeholder reuse).
//
// Map keys sort — "test.A*" parses before "test.Z*", forcing the
// consumer-first order that used to break.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <map>
#include <string>

using cajeta_test::CajetaJit;

// A class parsed BEFORE the view it takes as a parameter: member access
// through the (formerly propertyless) forward reference.
TEST(ViewForwardReferenceTests, forwardViewParamMemberAccess) {
    std::map<std::string, std::string> sources;
    sources["test.AConsumer"] =
        "package test;\n"
        "public final class AConsumer {\n"
        "    public static int64 f(ZView v) { return v.s; }\n"
        "    public static int32 run() {\n"
        "        int32[] b = heap int32[3];\n"
        "        b[0] = 5;\n"
        "        b[1] = 4;\n"
        "        b[2] = 1684234849;\n"
        "        ZView v = ZView(b);\n"
        "        if (AConsumer.f(v) != 5) return 10;\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    sources["test.ZView"] =
        "package test;\n"
        "@HostEndian\n"
        "public view ZView {\n"
        "    public int32  s;\n"
        "    public String name;\n"
        "}\n";
    auto jit = CajetaJit::compile(sources, "test.AConsumer");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// A view parsed BEFORE its element view: `ZDelta[]` must classify as an
// element-array field (descriptor view, per-element access) even though
// ZDelta is a forward reference at AMsg's prototype time.
TEST(ViewForwardReferenceTests, forwardElementViewClassified) {
    std::map<std::string, std::string> sources;
    sources["test.AMsg"] =
        "package test;\n"
        "@HostEndian\n"
        "public view AMsg {\n"
        "    public int32    magic;\n"
        "    public ZDelta[] ds;\n"
        "}\n";
    sources["test.BUse"] =
        "package test;\n"
        "public final class BUse {\n"
        "    public static int32 run() {\n"
        "        int32[] b = heap int32[8];\n"
        "        b[0] = 7;\n"
        "        b[1] = 2;\n"
        "        b[2] = 5;\n"
        "        b[3] = 4;\n"
        "        b[4] = 1684234849;\n"     // \"abcd\"
        "        b[5] = 9;\n"
        "        b[6] = 4;\n"
        "        b[7] = 2054781047;\n"     // \"wxyz\"
        "        AMsg m = AMsg(b);\n"
        "        if (m.ds.count() != 2) return 10;\n"
        "        if (m.ds[1].s != 9) return 11;\n"
        "        String n = m.ds[1].name;\n"
        "        if (!n.equals(\"wxyz\")) return 12;\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    sources["test.ZDelta"] =
        "package test;\n"
        "@HostEndian\n"
        "public view ZDelta {\n"
        "    public int32  s;\n"
        "    public String name;\n"
        "}\n";
    auto jit = CajetaJit::compile(sources, "test.BUse");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}
