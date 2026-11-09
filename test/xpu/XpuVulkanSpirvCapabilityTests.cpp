// The runtime refuses a SPIR-V module that declares a capability the device
// did not enable, by name, before the driver compiles it (benchmark-reference
// 14.3.7: RADV segfaults on such a module instead of rejecting it). Pure scan
// of the module's OpCapability prologue; no device.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

extern "C" uint32_t __cajeta_xpu_vk_spv_missing_capability(const uint32_t* w, size_t nwords,
                                                            uint32_t have, const char** name);

namespace {

constexpr uint32_t kFloat16 = 1u << 0;
constexpr uint32_t kInt8 = 1u << 1;
constexpr uint32_t kInt64 = 1u << 3;
constexpr uint32_t kStorage16 = 1u << 5;
constexpr uint32_t kCoopMat = 1u << 6;

// A module header, the capabilities given, OpMemoryModel, then a stray
// OpCapability the prologue scan must not reach.
std::vector<uint32_t> module(std::initializer_list<uint32_t> caps) {
    std::vector<uint32_t> w = {0x07230203u, 0x00010300u, 0, 100, 0};
    for (uint32_t c : caps) {
        w.push_back((2u << 16) | 17u);   // OpCapability
        w.push_back(c);
    }
    w.push_back((3u << 16) | 14u);       // OpMemoryModel Logical GLSL450
    w.push_back(0);
    w.push_back(1);
    w.push_back((2u << 16) | 17u);
    w.push_back(9);
    return w;
}

}  // namespace

TEST(XpuVulkanSpirvCapabilityTests, float16DeclaredButNotEnabledIsNamed) {
    auto w = module({1 /*Shader*/, 9 /*Float16*/});
    const char* name = nullptr;
    EXPECT_EQ(9u, __cajeta_xpu_vk_spv_missing_capability(w.data(), w.size(), kInt8 | kInt64, &name));
    ASSERT_NE(nullptr, name);
    EXPECT_STREQ("Float16", name);
}

TEST(XpuVulkanSpirvCapabilityTests, enabledCapabilitiesPass) {
    auto w = module({1, 9, 39 /*Int8*/, 4433 /*StorageBuffer16BitAccess*/, 6022 /*CooperativeMatrixKHR*/});
    const char* name = nullptr;
    EXPECT_EQ(0u, __cajeta_xpu_vk_spv_missing_capability(
                      w.data(), w.size(), kFloat16 | kInt8 | kStorage16 | kCoopMat, &name));
    EXPECT_EQ(nullptr, name);
}

TEST(XpuVulkanSpirvCapabilityTests, theFirstMissingOneIsReportedInModuleOrder) {
    auto w = module({1, 4433, 6022});
    const char* name = nullptr;
    EXPECT_EQ(4433u, __cajeta_xpu_vk_spv_missing_capability(w.data(), w.size(), kCoopMat, &name));
    EXPECT_STREQ("StorageBuffer16BitAccess", name);
    EXPECT_EQ(6022u, __cajeta_xpu_vk_spv_missing_capability(w.data(), w.size(), kStorage16, &name));
    EXPECT_STREQ("CooperativeMatrixKHR", name);
}

TEST(XpuVulkanSpirvCapabilityTests, unknownCapabilitiesAndBadModulesPass) {
    // A capability the runtime does not track (Shader, GroupNonUniform) is the
    // driver's to judge; a module that is not SPIR-V is left to vkCreateShaderModule.
    auto w = module({1, 61 /*GroupNonUniform*/});
    const char* name = nullptr;
    EXPECT_EQ(0u, __cajeta_xpu_vk_spv_missing_capability(w.data(), w.size(), 0, &name));
    std::vector<uint32_t> junk = {1, 2, 3};
    EXPECT_EQ(0u, __cajeta_xpu_vk_spv_missing_capability(junk.data(), junk.size(), 0, &name));
    EXPECT_EQ(0u, __cajeta_xpu_vk_spv_missing_capability(nullptr, 0, 0, &name));
}
