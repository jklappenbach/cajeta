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

// ── The live half: what this machine actually selected ──────────────────────
//
// apple-vulkan 4.1.4 / spec 3.7. Assertions are env-gated so the suite stays
// green anywhere, and CI turns them into a real gate by naming what it expects.
// This is also how the [hw] pair 2.3.1/2.3.2 gets checked once a Mac exists:
// CAJETA_XPU_BACKEND=vulkan CAJETA_EXPECT_VK_DRIVER_ID=28 cajeta_test --gtest_filter=...
#include <cstdlib>
#include <cstring>

extern "C" {
void        __cajeta_xpu_register_backend(int32_t id);
int32_t     __cajeta_xpu_device_supports(int32_t cap);
int32_t     __cajeta_xpu_vk_built(void);
int32_t     __cajeta_xpu_vk_init_status(void);
uint32_t    __cajeta_xpu_vk_driver_id(void);
const char* __cajeta_xpu_vk_driver_name(void);
const char* __cajeta_xpu_vk_driver_info(void);
}

namespace {
constexpr int32_t CAJ_XPU_VULKAN = 2;   // cajeta_xpu_dispatch.c's backend id

// Backends are registered by a ctor the COMPILER emits per bundled backend, and
// this binary was linked without one — so the dispatch layer sees an empty
// bundle and never initializes anything. Register Vulkan by hand, then touch a
// capability, which resolves and initializes the active backend.
//
// ORDER MATTERS: the dispatch layer caches the active backend on the FIRST
// capability touch anywhere in the process. If another suite got there first it
// cached "none", and registering afterwards is too late. So the two live probes
// below only report a device when this filter runs on its own — which is how CI
// runs them, and what the assertion message tells you to do.
void forceBackendInit() {
    __cajeta_xpu_register_backend(CAJ_XPU_VULKAN);
    (void) __cajeta_xpu_device_supports(0);
}
// A set-but-empty env var means "no expectation" — CI writes one per leg.
const char* expectation(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
}
} // namespace

TEST(XpuVulkanDriverSelect, reportsTheSelectedDriver) {
    if (!__cajeta_xpu_vk_built()) GTEST_SKIP() << "built without Vulkan headers";
    forceBackendInit();
    const int32_t status = __cajeta_xpu_vk_init_status();
    std::fprintf(stderr, "vk: status=%d driverId=%u name='%s' info='%s'\n",
                 status, __cajeta_xpu_vk_driver_id(),
                 __cajeta_xpu_vk_driver_name(), __cajeta_xpu_vk_driver_info());

    // Whatever happened, it is one of the three outcomes §3.5 names.
    EXPECT_TRUE(status == 0 || status == INCOMPATIBLE || status == INIT_FAILED)
        << "unexpected init status " << status;
    if (status == 0)
        EXPECT_STRNE(__cajeta_xpu_vk_driver_name(), "")
            << "a live device must name its driver (§3.7)";

    if (const char* want = expectation("CAJETA_EXPECT_VK_INIT_STATUS"))
        EXPECT_EQ(status, std::atoi(want));
    if (const char* want = expectation("CAJETA_EXPECT_VK_DRIVER_ID")) {
        ASSERT_EQ(status, 0)
            << "expected driver " << want << " but Vulkan did not come up. If "
               "other suites ran first they cached the active backend as none; "
               "run with --gtest_filter='XpuVulkanDriverSelect.*' alone.";
        EXPECT_EQ(__cajeta_xpu_vk_driver_id(), (uint32_t) std::atoi(want));
    }
}

// spec 4.6 — the degrade path's runtime input. Neither Apple driver advertises
// VK_KHR_shader_atomic_int64, so both must answer 0 here; a discrete Vulkan GPU
// answers 1. Off Vulkan the answer is unconditionally 1 (CPU/CUDA/HIP).
TEST(XpuVulkanDriverSelect, reportsAtomicInt64Support) {
    forceBackendInit();
    const int32_t v = __cajeta_xpu_device_supports(3);
    std::fprintf(stderr, "vk: AtomicInt64=%d\n", v);
    EXPECT_TRUE(v == 0 || v == 1);
    if (const char* want = expectation("CAJETA_EXPECT_ATOMIC_INT64"))
        EXPECT_EQ(v, std::atoi(want));
}
