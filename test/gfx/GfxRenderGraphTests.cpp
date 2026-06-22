//
// GfxRenderGraphTests — cajeta-gfx plan §5 (slice 1): the minimal render-graph's
// DEPENDENCY + HAZARD policy (cajeta.gpu.gfx.RenderGraph).
//
// A render graph is opinion-free: passes declare the resources they read and
// write, and the graph derives (a) the dependencies / a correct execution order
// and (b) the barriers between passes. The atomic decision underneath all of that
// is pure set logic over each pass's read/write sets — does a later pass conflict
// with an earlier one, what KIND of hazard (read-after-write, write-after-read,
// write-after-write), and therefore whether a memory barrier (cache flush +
// invalidate) is needed or a bare execution barrier suffices. That is the
// verifiable-here core of TDD 5.a/5.b; the live VkCmdPipelineBarrier recording is
// device-machine work.
//
// Resource sets are int32 BITMASKS (resource r -> bit r, up to 32 resources in v1),
// matching how a render graph fingerprints pass I/O. Hazard kinds are a small bit
// enum: RAW=1, WAR=2, WAW=4. Each test JITs a tiny program returning 0 on success
// or a negative failure code.
//
// GPU-free (pure host JIT; no device).
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& imports, const std::string& body) {
    std::string src =
        "package test;\n"
        + imports +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + body +
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* IMP = "import cajeta.gpu.gfx.RenderGraph;\n";

} // namespace

// 5a — dependency detection: a later pass depends on an earlier one iff their
// read/write sets conflict on any resource (RAW, WAR, or WAW). Disjoint passes are
// independent (no dependency → reorderable / concurrent). resourceBit(r) builds the
// single-resource mask 1<<r.
TEST(GfxRenderGraphTests, dependencyDetectsConflictsAndIndependence) {
    EXPECT_EQ(runI32(IMP,
        "        int32 r0 = RenderGraph.resourceBit(0);\n"
        "        int32 r1 = RenderGraph.resourceBit(1);\n"
        "        int32 r2 = RenderGraph.resourceBit(2);\n"
        // prior writes r0; next reads r0 -> RAW dependency.
        "        if (!RenderGraph.dependsOn(0, r0, r0, 0)) { return -1; }\n"
        // prior reads r1; next writes r1 -> WAR dependency.
        "        if (!RenderGraph.dependsOn(r1, 0, 0, r1)) { return -2; }\n"
        // both write r2 -> WAW dependency.
        "        if (!RenderGraph.dependsOn(0, r2, 0, r2)) { return -3; }\n"
        // prior touches r0, next touches r1 (disjoint) -> independent.
        "        if (RenderGraph.dependsOn(r0, r0, r1, r1)) { return -4; }\n"
        // both only READ r0 (no write) -> no hazard, independent.
        "        if (RenderGraph.dependsOn(r0, 0, r0, 0)) { return -5; }\n"
        "        return 0;\n"), 0);
}

// 5a — hazard classification: hazardMask sets RAW(1) when next reads a prior
// write, WAR(2) when next writes a prior read, WAW(4) when both write — and
// combines them. Two read-only passes have no hazard (0).
TEST(GfxRenderGraphTests, hazardMaskClassifiesKinds) {
    EXPECT_EQ(runI32(IMP,
        "        int32 r0 = RenderGraph.resourceBit(0);\n"
        "        if (RenderGraph.hazardMask(0, r0, r0, 0) != RenderGraph.hazardRaw()) { return -1; }\n"
        "        if (RenderGraph.hazardMask(r0, 0, 0, r0) != RenderGraph.hazardWar()) { return -2; }\n"
        "        if (RenderGraph.hazardMask(0, r0, 0, r0) != RenderGraph.hazardWaw()) { return -3; }\n"
        // prior writes r0; next both reads r0 (RAW) and writes r0 (WAW) -> RAW|WAW = 1|4 = 5.
        "        if (RenderGraph.hazardMask(0, r0, r0, r0) != 5) { return -4; }\n"
        "        if (RenderGraph.hazardMask(r0, 0, r0, 0) != 0) { return -5; }\n"  // both read-only
        "        if (RenderGraph.hazardRaw() != 1) { return -6; }\n"
        "        if (RenderGraph.hazardWar() != 2) { return -7; }\n"
        "        if (RenderGraph.hazardWaw() != 4) { return -8; }\n"
        "        return 0;\n"), 0);
}

// 5a — barrier kind from the hazard: RAW and WAW need a MEMORY barrier (the
// producer's writes must be made available + visible — cache flush/invalidate);
// a WAR hazard alone needs only an EXECUTION barrier (ordering, no cache op).
TEST(GfxRenderGraphTests, memoryBarrierForRawWawNotWar) {
    EXPECT_EQ(runI32(IMP,
        "        if (!RenderGraph.needsMemoryBarrier(RenderGraph.hazardRaw())) { return -1; }\n"
        "        if (!RenderGraph.needsMemoryBarrier(RenderGraph.hazardWaw())) { return -2; }\n"
        "        if (RenderGraph.needsMemoryBarrier(RenderGraph.hazardWar())) { return -3; }\n"  // exec-only
        "        if (RenderGraph.needsMemoryBarrier(0)) { return -4; }\n"                         // no hazard
        // WAR|RAW -> the RAW still forces a memory barrier.
        "        int32 mix = RenderGraph.hazardWar() | RenderGraph.hazardRaw();\n"
        "        if (!RenderGraph.needsMemoryBarrier(mix)) { return -5; }\n"
        "        return 0;\n"), 0);
}

// 5a — last-writer tracking: a consumer of a resource must synchronize against the
// NEAREST earlier pass that wrote it (not every prior writer). lastWriterBefore
// scans passes [0, beforePass) and returns the highest index writing the resource,
// or -1 when none produced it (an external / first-use resource).
TEST(GfxRenderGraphTests, lastWriterTracksNearestProducer) {
    EXPECT_EQ(runI32(IMP,
        "        int32 r0 = RenderGraph.resourceBit(0);\n"
        "        int32 r1 = RenderGraph.resourceBit(1);\n"
        "        int32[] writes = heap int32[4];\n"
        "        writes[0] = r0;\n"   // pass 0 writes r0
        "        writes[1] = r1;\n"   // pass 1 writes r1
        "        writes[2] = r0;\n"   // pass 2 writes r0 again
        "        writes[3] = 0;\n"    // pass 3 writes nothing
        // consumer at pass 3 reading r0 -> nearest earlier writer is pass 2.
        "        if (RenderGraph.lastWriterBefore(writes, 4, r0, 3) != 2) { return -1; }\n"
        // consumer at pass 2 reading r0 -> only pass 0 wrote it earlier.
        "        if (RenderGraph.lastWriterBefore(writes, 4, r0, 2) != 0) { return -2; }\n"
        // r1's producer is pass 1.
        "        if (RenderGraph.lastWriterBefore(writes, 4, r1, 3) != 1) { return -3; }\n"
        // resource bit 2 never written -> -1 (external / first use).
        "        if (RenderGraph.lastWriterBefore(writes, 4, RenderGraph.resourceBit(2), 3) != -1) { return -4; }\n"
        // beforePass 0 -> nothing earlier -> -1.
        "        if (RenderGraph.lastWriterBefore(writes, 4, r0, 0) != -1) { return -5; }\n"
        "        return 0;\n"), 0);
}
