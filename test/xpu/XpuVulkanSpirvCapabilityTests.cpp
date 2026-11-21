// The runtime refuses a SPIR-V module that declares a capability the device
// did not enable, by name, before the driver compiles it (benchmark-reference
// 14.3.7: RADV segfaults on such a module instead of rejecting it). Pure scan
// of the module's OpCapability prologue; no device.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
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

// ---- the 16x16x16 configs a module's tiles need (gpu-kernel-parity 14.2.6) ----------
//
// CooperativeMatrixKHR says a device has matrix units, not which shapes and types.
// The 4090's Vulkan lists int8 only at 16x16x32, and a native 16x16x16 int8 tile
// launched there wrote zeros silently. A module whose tile the device does not list
// is reported like a missing capability, so the launcher takes "$pt" or refuses.

namespace {

constexpr uint32_t kCoopF16F32 = 1u << 11;
constexpr uint32_t kCoopS8 = 1u << 12;
constexpr uint32_t kCoopBf16 = 1u << 13;
constexpr uint32_t kCoopF16F16 = 1u << 14;
constexpr uint32_t kAllCoop = kCoopMat | kCoopF16F32 | kCoopS8 | kCoopBf16 | kCoopF16F16;

// A tile's element: an int of `width`, a float of `width`, or bf16.
struct Elem { bool isFloat; uint32_t width; bool bf16; };
constexpr Elem kS8{false, 8, false}, kS32{false, 32, false}, kF16{true, 16, false},
    kF32{true, 32, false}, kBf16{true, 16, true};

// A module declaring CooperativeMatrixKHR and one 16x16 subgroup tile per {elem, use},
// then a function the type scan must stop at.
std::vector<uint32_t> tileModule(std::initializer_list<std::pair<Elem, uint32_t>> tiles,
                                 uint32_t rows = 16) {
    std::vector<uint32_t> w = {0x07230203u, 0x00010600u, 0, 1000, 0};
    for (uint32_t c : {1u, 6022u}) { w.push_back((2u << 16) | 17u); w.push_back(c); }
    w.insert(w.end(), {(3u << 16) | 14u, 0, 1});            // OpMemoryModel
    uint32_t id = 1;
    const uint32_t i32 = id++;
    w.insert(w.end(), {(4u << 16) | 21u, i32, 32, 1});       // OpTypeInt 32 signed
    auto constant = [&](uint32_t v) {
        uint32_t c = id++;
        w.insert(w.end(), {(4u << 16) | 43u, i32, c, v});     // OpConstant
        return c;
    };
    const uint32_t scope = constant(3), r = constant(rows), c16 = constant(16);
    for (const auto& [e, use] : tiles) {
        uint32_t t = id++;
        if (!e.isFloat) w.insert(w.end(), {(4u << 16) | 21u, t, e.width, 1});
        else if (e.bf16) w.insert(w.end(), {(4u << 16) | 22u, t, 16, 0});   // BFloat16KHR
        else w.insert(w.end(), {(3u << 16) | 22u, t, e.width});
        const uint32_t u = constant(use);
        w.insert(w.end(), {(7u << 16) | 4456u, id++, t, scope, r, c16, u});
    }
    w.insert(w.end(), {(5u << 16) | 54u, i32, id++, 0, i32});   // OpFunction
    return w;
}

}  // namespace

TEST(XpuVulkanSpirvCapabilityTests, anInt8TileTheDeviceDoesNotListIsNamed) {
    auto w = tileModule({{kS8, 0}, {kS8, 1}, {kS32, 2}});
    const char* name = nullptr;
    EXPECT_NE(0u, __cajeta_xpu_vk_spv_missing_capability(w.data(), w.size(),
                                                         kAllCoop & ~kCoopS8, &name));
    ASSERT_NE(nullptr, name);
    EXPECT_NE(std::string(name).find("int8"), std::string::npos) << name;
    EXPECT_EQ(0u, __cajeta_xpu_vk_spv_missing_capability(w.data(), w.size(), kAllCoop, &name));
}

TEST(XpuVulkanSpirvCapabilityTests, eachTileTypeNeedsItsOwnConfig) {
    const char* name = nullptr;
    auto f16f32 = tileModule({{kF16, 0}, {kF16, 1}, {kF32, 2}});
    EXPECT_NE(0u, __cajeta_xpu_vk_spv_missing_capability(f16f32.data(), f16f32.size(),
                                                         kAllCoop & ~kCoopF16F32, &name));
    EXPECT_EQ(0u, __cajeta_xpu_vk_spv_missing_capability(f16f32.data(), f16f32.size(),
                                                         kCoopMat | kCoopF16F32, &name));
    auto f16f16 = tileModule({{kF16, 0}, {kF16, 1}, {kF16, 2}});
    EXPECT_NE(0u, __cajeta_xpu_vk_spv_missing_capability(f16f16.data(), f16f16.size(),
                                                         kAllCoop & ~kCoopF16F16, &name));
    auto bf16 = tileModule({{kBf16, 0}, {kBf16, 1}, {kF32, 2}});
    EXPECT_NE(0u, __cajeta_xpu_vk_spv_missing_capability(bf16.data(), bf16.size(),
                                                         kAllCoop & ~kCoopBf16, &name));
    EXPECT_NE(std::string(name).find("bfloat16"), std::string::npos) << name;
    EXPECT_EQ(0u, __cajeta_xpu_vk_spv_missing_capability(bf16.data(), bf16.size(),
                                                         kCoopMat | kCoopBf16 | kCoopF16F32, &name));
}

// The capability itself is still checked first, and a module with no tiles needs no config.
TEST(XpuVulkanSpirvCapabilityTests, theCapabilityComesFirstAndTilelessModulesNeedNoConfig) {
    const char* name = nullptr;
    auto w = tileModule({{kS8, 0}});
    EXPECT_EQ(6022u, __cajeta_xpu_vk_spv_missing_capability(w.data(), w.size(), kCoopS8, &name));
    EXPECT_STREQ("CooperativeMatrixKHR", name);
    auto none = tileModule({});
    EXPECT_EQ(0u, __cajeta_xpu_vk_spv_missing_capability(none.data(), none.size(), kCoopMat, &name));
}
