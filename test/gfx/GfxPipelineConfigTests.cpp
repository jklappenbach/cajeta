//
// GfxPipelineConfigTests — cajeta-gfx plan §4.d (slice 1): the graphics
// PIPELINE-STATE SELECTION POLICY (cajeta.gpu.gfx.PipelineConfig).
//
// A graphics pipeline is built from a description — vertex input layout, input
// assembly, raster, blend, depth/stencil. Deriving that description from a
// renderer's intent (attribute sizes, a blend preset, reverse-Z, double-sided) is
// pure, backend-independent arithmetic over VK_* enum ordinals — the verifiable-
// here core of §4.d. The live VkPipeline / render-pass / framebuffer creation and
// the draw commands need a device and are exercised on the device machine (like
// the §4.b SPIR-V binaries and the §4.c swapchain present loop).
//
// Ordinals are the stable native contract (as for SwapchainConfig / TextureFormat):
// VkBlendFactor (ZERO=0, ONE=1, SRC_ALPHA=6, ONE_MINUS_SRC_ALPHA=7), VkCompareOp
// (LESS_OR_EQUAL=3, GREATER_OR_EQUAL=6), VkCullModeFlagBits (NONE=0, BACK=2),
// VkFrontFace (COUNTER_CLOCKWISE=0), VkPrimitiveTopology (TRIANGLE_LIST=3). Each
// test JITs a tiny program returning 0 on success or a negative failure code.
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

const char* IMP = "import cajeta.gpu.gfx.PipelineConfig;\n";

} // namespace

// 4d — vertex-input layout: a packed/interleaved vertex binding lays its
// attributes back-to-back, so each attribute's byte offset is the prefix sum of
// the earlier attribute sizes and the binding stride is the total. (A vec3
// position = 12 B, vec3 normal = 12 B, vec2 uv = 8 B → offsets 0/12/24, stride 32.)
TEST(GfxPipelineConfigTests, vertexLayoutPacksOffsetsAndStride) {
    EXPECT_EQ(runI32(IMP,
        "        int32[] sz = heap int32[3];\n"
        "        sz[0] = 12; sz[1] = 12; sz[2] = 8;\n"
        "        if (PipelineConfig.vertexAttributeOffset(sz, 3, 0) != 0)  { return -1; }\n"
        "        if (PipelineConfig.vertexAttributeOffset(sz, 3, 1) != 12) { return -2; }\n"
        "        if (PipelineConfig.vertexAttributeOffset(sz, 3, 2) != 24) { return -3; }\n"
        "        if (PipelineConfig.vertexAttributeOffset(sz, 3, 3) != -1) { return -4; }\n"  // out of range
        "        if (PipelineConfig.vertexStride(sz, 3) != 32) { return -5; }\n"
        "        return 0;\n"), 0);
}

// 4d — blend presets map to the VkBlendFactor pair (src, dst): OPAQUE = (ONE,
// ZERO); ALPHA = (SRC_ALPHA, ONE_MINUS_SRC_ALPHA); PREMULTIPLIED = (ONE,
// ONE_MINUS_SRC_ALPHA); ADDITIVE = (ONE, ONE).
TEST(GfxPipelineConfigTests, blendPresetsMapToVkFactors) {
    EXPECT_EQ(runI32(IMP,
        "        if (PipelineConfig.blendSrcFactor(0) != 1) { return -1; }\n"   // OPAQUE src ONE
        "        if (PipelineConfig.blendDstFactor(0) != 0) { return -2; }\n"   // OPAQUE dst ZERO
        "        if (PipelineConfig.blendSrcFactor(1) != 6) { return -3; }\n"   // ALPHA src SRC_ALPHA
        "        if (PipelineConfig.blendDstFactor(1) != 7) { return -4; }\n"   // ALPHA dst 1-SRC_ALPHA
        "        if (PipelineConfig.blendSrcFactor(2) != 1) { return -5; }\n"   // PREMULT src ONE
        "        if (PipelineConfig.blendDstFactor(2) != 7) { return -6; }\n"   // PREMULT dst 1-SRC_ALPHA
        "        if (PipelineConfig.blendSrcFactor(3) != 1) { return -7; }\n"   // ADDITIVE src ONE
        "        if (PipelineConfig.blendDstFactor(3) != 1) { return -8; }\n"   // ADDITIVE dst ONE
        "        return 0;\n"), 0);
}

// 4d — depth policy: the compare op is LESS_OR_EQUAL (3) for a standard [0,1]
// depth buffer and GREATER_OR_EQUAL (6) for a reverse-Z buffer; depth WRITE is on
// only for the opaque pass (transparent passes test against, but do not overwrite,
// the depth buffer).
TEST(GfxPipelineConfigTests, depthPolicyCompareAndWrite) {
    EXPECT_EQ(runI32(IMP,
        "        if (PipelineConfig.depthCompareOp(false) != 3) { return -1; }\n"  // standard LEQUAL
        "        if (PipelineConfig.depthCompareOp(true)  != 6) { return -2; }\n"  // reverse-Z GEQUAL
        "        if (!PipelineConfig.depthWriteEnabled(0)) { return -3; }\n"       // OPAQUE writes
        "        if (PipelineConfig.depthWriteEnabled(1))  { return -4; }\n"       // ALPHA does not
        "        if (PipelineConfig.depthWriteEnabled(2))  { return -5; }\n"       // PREMULT does not
        "        if (PipelineConfig.depthWriteEnabled(3))  { return -6; }\n"       // ADDITIVE does not
        "        return 0;\n"), 0);
}

// 4d — raster + input-assembly defaults: back-face cull for a one-sided mesh,
// no cull for a double-sided one; the front face is counter-clockwise (matching
// cajeta.math Camera's RH / [0,1]-clip convention); the default topology is a
// triangle list.
TEST(GfxPipelineConfigTests, rasterAndInputAssemblyDefaults) {
    EXPECT_EQ(runI32(IMP,
        "        if (PipelineConfig.cullModeFor(false) != 2) { return -1; }\n"   // one-sided -> BACK
        "        if (PipelineConfig.cullModeFor(true)  != 0) { return -2; }\n"   // double-sided -> NONE
        "        if (PipelineConfig.frontFaceCounterClockwise() != 0) { return -3; }\n"
        "        if (PipelineConfig.topologyTriangleList() != 3) { return -4; }\n"
        "        return 0;\n"), 0);
}

// 4d (slice 2) — VkFormat byte sizing: the R32 families size by channel count
// (4/8/12/16), packed RGBA8_UNORM is 4, and the 16-bit floats size 2/4/8; an
// unsupported format returns -1. Composes with vertexStride (formats → bytes →
// stride): a vec3(R32G32B32_SFLOAT) + a packed RGBA8 color = 12 + 4 = 16.
TEST(GfxPipelineConfigTests, vertexFormatBytesMapsVkFormats) {
    EXPECT_EQ(runI32(IMP,
        "        if (PipelineConfig.vertexFormatBytes(100) != 4)  { return -1; }\n"  // R32_SFLOAT
        "        if (PipelineConfig.vertexFormatBytes(103) != 8)  { return -2; }\n"  // R32G32_SFLOAT
        "        if (PipelineConfig.vertexFormatBytes(106) != 12) { return -3; }\n"  // R32G32B32_SFLOAT
        "        if (PipelineConfig.vertexFormatBytes(109) != 16) { return -4; }\n"  // R32G32B32A32_SFLOAT
        "        if (PipelineConfig.vertexFormatBytes(37)  != 4)  { return -5; }\n"  // R8G8B8A8_UNORM
        "        if (PipelineConfig.vertexFormatBytes(76)  != 2)  { return -6; }\n"  // R16_SFLOAT
        "        if (PipelineConfig.vertexFormatBytes(97)  != 8)  { return -7; }\n"  // R16G16B16A16_SFLOAT
        "        if (PipelineConfig.vertexFormatBytes(9999) != -1) { return -8; }\n" // unsupported
        "        int32[] b = heap int32[2];\n"
        "        b[0] = PipelineConfig.vertexFormatBytes(106);\n"  // 12
        "        b[1] = PipelineConfig.vertexFormatBytes(37);\n"   // 4
        "        if (PipelineConfig.vertexStride(b, 2) != 16) { return -9; }\n"
        "        return 0;\n"), 0);
}

// 4d (slice 2) — viewport Y-flip: with flipY the origin moves to the bottom edge
// (y = extentHeight) and the height goes negative (the VK_KHR_maintenance1 trick
// for an RH/Y-up renderer); without it the viewport is the plain top-left origin.
TEST(GfxPipelineConfigTests, viewportYFlipPolicy) {
    EXPECT_EQ(runI32(IMP,
        "        if (PipelineConfig.viewportY(720, false) != 0)      { return -1; }\n"
        "        if (PipelineConfig.viewportHeight(720, false) != 720) { return -2; }\n"
        "        if (PipelineConfig.viewportY(720, true) != 720)     { return -3; }\n"
        "        if (PipelineConfig.viewportHeight(720, true) != -720) { return -4; }\n"
        "        return 0;\n"), 0);
}

// 4d (slice 2) — render-pass attachment ops: a color target clears (LOAD_OP_CLEAR
// 1) or is fully overwritten (DONT_CARE 2), and stores (0) only when kept; depth
// clears at frame start and is usually discarded after the pass (STORE_OP DONT_CARE
// 1). VkAttachmentLoadOp {LOAD 0, CLEAR 1, DONT_CARE 2}; StoreOp {STORE 0, DONT_CARE 1}.
TEST(GfxPipelineConfigTests, attachmentLoadStoreOps) {
    EXPECT_EQ(runI32(IMP,
        "        if (PipelineConfig.colorLoadOp(true)  != 1) { return -1; }\n"   // CLEAR
        "        if (PipelineConfig.colorLoadOp(false) != 2) { return -2; }\n"   // DONT_CARE
        "        if (PipelineConfig.colorStoreOp(true)  != 0) { return -3; }\n"  // STORE (presented)
        "        if (PipelineConfig.colorStoreOp(false) != 1) { return -4; }\n"  // DONT_CARE
        "        if (PipelineConfig.depthLoadOp(true)  != 1) { return -5; }\n"   // CLEAR
        "        if (PipelineConfig.depthStoreOp(false) != 1) { return -6; }\n"  // discarded after pass
        "        if (PipelineConfig.depthStoreOp(true)  != 0) { return -7; }\n"  // STORE (reused later)
        "        return 0;\n"), 0);
}
