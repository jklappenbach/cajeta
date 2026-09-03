// apple-vulkan 2.1 — which Vulkan-on-Metal ICD wins (spec §3.1–§3.6).
//
// Pure policy, no device. A Mac carrying both KosmicKrisp and MoltenVK is the
// case that matters and no Linux machine can produce it; the API half —
// querying VkPhysicalDeviceDriverProperties, reporting the winner — is the
// [hw] acceptance pair 2.3.1/2.3.2.
#include "gtest/gtest.h"
#include <cstdint>

extern "C" {
int32_t __cajeta_xpu_vk_pick_device(const uint32_t* driverIds, int32_t n,
                                    const char* force);
int32_t __cajeta_xpu_vk_classify_init(int32_t loaderFound, int32_t deviceCount);
}

namespace {
// VkDriverId, spelled raw: the runtime's pure layer carries no Vulkan types.
constexpr uint32_t KK   = 28;   // VK_DRIVER_ID_MESA_KOSMICKRISP
constexpr uint32_t MVK  = 14;   // VK_DRIVER_ID_MOLTENVK
constexpr uint32_t RADV = 3;    // VK_DRIVER_ID_MESA_RADV
constexpr int32_t INCOMPATIBLE = -9;   // VK_ERROR_INCOMPATIBLE_DRIVER
constexpr int32_t INIT_FAILED  = -3;   // VK_ERROR_INITIALIZATION_FAILED
} // namespace

// 2.1.1 / §3.3 — KosmicKrisp is Vulkan 1.3 conformant; MoltenVK is not.
TEST(XpuVulkanDriverSelect, prefersKosmicKrispOverMoltenVk) {
    const uint32_t ids[2] = { MVK, KK };
    EXPECT_EQ(__cajeta_xpu_vk_pick_device(ids, 2, nullptr), 1);
}

// 2.1.4 / §3.2 — the loader does no physical-device sorting on macOS, so the
// two ICDs may arrive either way round and must still yield the same driver.
TEST(XpuVulkanDriverSelect, choiceIsIndependentOfEnumerationOrder) {
    const uint32_t fwd[2] = { KK, MVK };
    const uint32_t rev[2] = { MVK, KK };
    EXPECT_EQ(fwd[__cajeta_xpu_vk_pick_device(fwd, 2, nullptr)],
              rev[__cajeta_xpu_vk_pick_device(rev, 2, nullptr)]);
}

// 2.1.2 / §3.4 — pre-macOS-26 or Intel: KosmicKrisp never enumerates.
TEST(XpuVulkanDriverSelect, selectsMoltenVkAloneWithoutError) {
    const uint32_t ids[1] = { MVK };
    EXPECT_EQ(__cajeta_xpu_vk_pick_device(ids, 1, nullptr), 0);
}

// 2.1.6 — off Apple no ID outranks another, so device 0 still wins.
TEST(XpuVulkanDriverSelect, leavesNonAppleSelectionUnchanged) {
    const uint32_t ids[2] = { RADV, RADV };
    EXPECT_EQ(__cajeta_xpu_vk_pick_device(ids, 2, nullptr), 0);
}

// 2.1.5 / §3.6
TEST(XpuVulkanDriverSelect, overrideBeatsThePolicy) {
    const uint32_t ids[2] = { KK, MVK };
    EXPECT_EQ(__cajeta_xpu_vk_pick_device(ids, 2, "moltenvk"), 1);
    EXPECT_EQ(__cajeta_xpu_vk_pick_device(ids, 2, "kosmickrisp"), 0);
}

// Naming an absent driver refuses, rather than quietly running on the other.
TEST(XpuVulkanDriverSelect, overrideForAnAbsentDriverRefuses) {
    const uint32_t ids[1] = { MVK };
    EXPECT_EQ(__cajeta_xpu_vk_pick_device(ids, 1, "kosmickrisp"), -1);
}

TEST(XpuVulkanDriverSelect, unsetOrUnknownOverrideFallsBackToThePolicy) {
    const uint32_t ids[2] = { MVK, KK };
    EXPECT_EQ(__cajeta_xpu_vk_pick_device(ids, 2, "nope"), 1);
    EXPECT_EQ(__cajeta_xpu_vk_pick_device(ids, 2, ""), 1);
}

TEST(XpuVulkanDriverSelect, refusesAnEmptyDeviceList) {
    EXPECT_EQ(__cajeta_xpu_vk_pick_device(nullptr, 0, nullptr), -1);
}

// 2.1.3 / §3.5 — "no Vulkan" has two causes needing different fixes.
TEST(XpuVulkanDriverSelect, separatesNoIcdFromNoDevice) {
    EXPECT_EQ(__cajeta_xpu_vk_classify_init(0, 0), INCOMPATIBLE);
    EXPECT_EQ(__cajeta_xpu_vk_classify_init(1, 0), INIT_FAILED);
    EXPECT_EQ(__cajeta_xpu_vk_classify_init(1, 1), 0);
}
