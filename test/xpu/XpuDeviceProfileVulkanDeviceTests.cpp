//
// CajetaXPU — the VULKAN branch of the live device query.
//
// CUDA and HIP each answer the machine model from a vendor attribute list. A
// box that has neither reaches its GPU through Vulkan, which is also the
// backend an UNKNOWN part arrives through — so before this branch existed
// every field of Device.multiprocessorCount() / registersPerMultiprocessor() /
// sharedBytesPerMultiprocessor() came back 0 there and each kernel fell back to
// a literal measured on one AMD part.
//
// What is asserted, and why it cannot pass vacuously: the first test drives
// cajeta_xpu_query_raw_device_vulkan directly and ASSERTs a 1 return with
// valid == 1, which a missing branch cannot produce. The portable fields have
// to be internally consistent (wave size a power of two, a workgroup at least
// one wave wide, a non-zero shared budget), and the facts core Vulkan simply
// does not carry have to be EXACTLY 0 — 0 is "unknown", and the caller's own
// measured constant standing is the whole point of not guessing.
//
// Skips cleanly with no Vulkan compute device (or a binary built without the
// Vulkan headers, which XpuDeviceTestUtil tells apart).
//

#include "gtest/gtest.h"

#include "cajeta_xpu_abi.h"
#include "XpuDeviceTestUtil.h"

#include <iostream>
#include <string>

namespace {

// A wave/subgroup width that could plausibly have come off a device: a power of
// two, and within the range real hardware spans (llvmpipe reports 8, RDNA 32,
// GCN 64). Zero is excluded on purpose — the portable field must be answered.
bool sanePowerOfTwoWave(uint32_t w) {
    return w >= 4u && w <= 128u && (w & (w - 1u)) == 0u;
}

} // namespace

// The portable core-Vulkan fields, on whatever device this box exposes.
TEST(XpuDeviceProfileVulkanDeviceTests, coreFieldsPopulated) {
    CAJETA_SKIP_IF_NO_VULKAN();
    CajetaXpuRawDevice raw;
    // Non-vacuous: with no Vulkan branch this returns 0 and the test fails here.
    ASSERT_EQ(cajeta_xpu_query_raw_device_vulkan(&raw), 1)
        << "the Vulkan device query answered nothing on a box with a Vulkan "
           "compute device";
    EXPECT_EQ(raw.valid, 1);

    std::cout << "[ device   ] vulkan: '" << raw.archName
              << "' wave=" << raw.waveSize
              << " maxThreads/block=" << raw.maxThreadsPerBlock
              << " lds/block=" << raw.ldsBytesPerBlock
              << " mp=" << raw.multiprocessorCount
              << " simds/mp=" << raw.simdsPerMP
              << " regs/mp=" << raw.regsPerMP
              << " threads/mp=" << raw.threadsPerMP
              << " vram=" << raw.totalGlobalMemBytes
              << " integrated=" << raw.integrated << "\n";

    // VkPhysicalDeviceProperties.deviceName — a NAME, not a gfx/sm token.
    EXPECT_NE(std::string(raw.archName), "");

    // VkPhysicalDeviceSubgroupProperties.subgroupSize, or the width the
    // pipeline path pins when subgroup-size control is usable.
    EXPECT_TRUE(sanePowerOfTwoWave(raw.waveSize))
        << "wave size " << raw.waveSize << " is not a plausible subgroup width";

    // maxComputeWorkGroupInvocations: a workgroup holds at least one wave.
    EXPECT_GE(raw.maxThreadsPerBlock, raw.waveSize);
    EXPECT_LE(raw.maxThreadsPerBlock, 4096u);

    // maxComputeWorkGroupSize[0] — never above the invocation cap (Vulkan
    // guarantees this, so a violation means the two came from different reads).
    EXPECT_GT(raw.maxBlockDimX, 0u);
    EXPECT_LE(raw.maxBlockDimX, raw.maxThreadsPerBlock);

    // maxComputeWorkGroupCount[0].
    EXPECT_GT(raw.maxGridDimX, 0u);

    // maxComputeSharedMemorySize: every compute-capable Vulkan device has one.
    EXPECT_GT(raw.ldsBytesPerBlock, 0u);

    // The DEVICE_LOCAL heaps, summed.
    EXPECT_GT(raw.totalGlobalMemBytes, 0ull);

    // integrated is a flag, not a measurement — 0 or 1, never a raw enum.
    EXPECT_TRUE(raw.integrated == 0 || raw.integrated == 1);
}

// 0 means UNKNOWN and is never a budget: core Vulkan carries none of these, and
// neither vendor property extension adds them, so every one must be exactly 0
// rather than a plausible-looking number borrowed from some other part.
TEST(XpuDeviceProfileVulkanDeviceTests, unreportableFieldsAreZero) {
    CAJETA_SKIP_IF_NO_VULKAN();
    CajetaXpuRawDevice raw;
    ASSERT_EQ(cajeta_xpu_query_raw_device_vulkan(&raw), 1);

    EXPECT_EQ(raw.ldsBytesPerBlockOptin, 0u);  // a CUDA opt-in cap, no analog
    EXPECT_EQ(raw.maxBlocksPerMP, 0u);         // not exposed by Vulkan
    EXPECT_EQ(raw.l2CacheBytes, 0u);           // not exposed by Vulkan
    EXPECT_EQ(raw.memoryClockKHz, 0u);         // not exposed by Vulkan
    EXPECT_EQ(raw.memoryBusWidthBits, 0u);     // not exposed by Vulkan
    EXPECT_EQ(raw.clockRateKHz, 0u);           // not exposed by Vulkan
    // VkPhysicalDeviceShaderCorePropertiesAMD carries no LDS size, and reading
    // the per-WORKGROUP cap as the per-MP budget is the substitution this rule
    // forbids: on NVIDIA the two differ by ~2x.
    EXPECT_EQ(raw.ldsBytesPerMP, 0u);
}

// The vendor half. Absent VK_AMD_shader_core_properties / VK_NV_shader_sm_builtins
// the whole occupancy group stays 0 together; present, the derived numbers have
// to agree with each other and with the wave width they were formed from.
TEST(XpuDeviceProfileVulkanDeviceTests, vendorGeometryIsSelfConsistent) {
    CAJETA_SKIP_IF_NO_VULKAN();
    CajetaXpuRawDevice raw;
    ASSERT_EQ(cajeta_xpu_query_raw_device_vulkan(&raw), 1);

    if (raw.multiprocessorCount == 0u) {
        // No vendor property extension: nothing derived from one may be set.
        EXPECT_EQ(raw.simdsPerMP, 0u);
        EXPECT_EQ(raw.regsPerMP, 0u);
        EXPECT_EQ(raw.threadsPerMP, 0u);
        GTEST_SKIP() << "no vendor shader-core extension on '" << raw.archName
                     << "' — the occupancy group stays unknown, as it should";
    }

    EXPECT_LE(raw.multiprocessorCount, 4096u);
    if (raw.threadsPerMP) {
        // Residency is (waves per SIMD) x (SIMDs per MP) x (wave width), so the
        // wave width must divide it exactly.
        EXPECT_EQ(raw.threadsPerMP % raw.waveSize, 0u);
        EXPECT_GE(raw.threadsPerMP, raw.waveSize);
    }
    if (raw.simdsPerMP) {
        // AMD: the register file is a whole number of registers per SIMD.
        EXPECT_GT(raw.regsPerMP, 0u);
        EXPECT_EQ(raw.regsPerMP % raw.simdsPerMP, 0u);
        EXPECT_EQ(raw.threadsPerMP % (raw.simdsPerMP * raw.waveSize), 0u);
    } else {
        // NVIDIA: Vulkan reports the SM count and warp residency and nothing
        // about the register file — which must therefore stay unknown.
        EXPECT_EQ(raw.regsPerMP, 0u);
    }
}

// With neither vendor runtime installed, the generic query must ROUTE to the
// Vulkan branch — the case the branch exists for. Skips where a CUDA or HIP
// stack is present and rightly answers first.
TEST(XpuDeviceProfileVulkanDeviceTests, genericQueryRoutesToVulkan) {
    CAJETA_SKIP_IF_NO_VULKAN();
    if (::cajeta::xpu::test::hipAvailable() || ::cajeta::xpu::test::cudaAvailable())
        GTEST_SKIP() << "a vendor runtime is installed and answers this query "
                        "from its own attribute list";
    CajetaXpuRawDevice vk, generic;
    ASSERT_EQ(cajeta_xpu_query_raw_device_vulkan(&vk), 1);
    ASSERT_EQ(cajeta_xpu_query_raw_device(&generic), 1)
        << "no CUDA/HIP and a Vulkan device present, yet the generic device "
           "query answered nothing";
    EXPECT_EQ(std::string(generic.archName), std::string(vk.archName));
    EXPECT_EQ(generic.waveSize, vk.waveSize);
    EXPECT_EQ(generic.ldsBytesPerBlock, vk.ldsBytesPerBlock);
    EXPECT_EQ(generic.multiprocessorCount, vk.multiprocessorCount);
}

// The convention check. multiprocessorCount is a UNIT, not just a number, and
// AMD's is ambiguous: RDNA pairs two CUs into a work-group processor and HIP
// reports WGPs. The Vulkan derivation folds CUs the same way, so on one part
// both backends must land on the same figures — otherwise cajeta would carry
// two conventions and a kernel's occupancy would depend on which backend it
// happened to dispatch through. Gated on this gfx1151 box (the same gate
// XpuDeviceProfileAmdDeviceTests uses), where both stacks see the same GPU.
TEST(XpuDeviceProfileVulkanDeviceTests, amdGeometryMatchesHipOnTheSamePart) {
    CAJETA_SKIP_IF_NO_VULKAN();
    CAJETA_SKIP_IF_NO_HIP();
    CajetaXpuRawDevice hip, vk;
    ASSERT_EQ(cajeta_xpu_query_raw_device(&hip), 1);
    if (std::string(hip.archName).rfind("gfx1151", 0) != 0)
        GTEST_SKIP() << "not the gfx1151 reference box: " << hip.archName;
    ASSERT_EQ(cajeta_xpu_query_raw_device_vulkan(&vk), 1);
    if (vk.simdsPerMP == 0u)
        GTEST_SKIP() << "the Vulkan device here is not the AMD part ('"
                     << vk.archName << "')";

    EXPECT_EQ(vk.waveSize, hip.waveSize);                        // 32
    EXPECT_EQ(vk.maxThreadsPerBlock, hip.maxThreadsPerBlock);    // 1024
    EXPECT_EQ(vk.multiprocessorCount, hip.multiprocessorCount);  // 20 WGPs
    EXPECT_EQ(vk.regsPerMP, hip.regsPerMP);                      // 196608
    EXPECT_EQ(vk.threadsPerMP, hip.threadsPerMP);                // 2048
    // The measured partition count the arch-NAME table cannot supply here.
    EXPECT_EQ(vk.simdsPerMP, 4u);
}
