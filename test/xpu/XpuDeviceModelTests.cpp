//
// DeviceModel — GPU-free tests for the smart-fallback candidate generator
// (kernel-occupancy-autotune §4): occupancy-based hard pruning + ordering and
// the linear conflict-free LDS pad, derived from gfx1151 properties.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/core/DeviceModel.h"

#include <algorithm>

using cajeta::xpu::DeviceModel;
using cajeta::xpu::conflictFreePad;

namespace {
DeviceModel gfx1151() { return DeviceModel{}; }   // defaults describe gfx1151
}

// A register-heavy kernel prunes the large blocks (they can't fit a workgroup at
// that VGPR demand) but keeps the small ones.
TEST(XpuDeviceModelTests, highVgprPrunesLargeBlocks) {
    DeviceModel d = gfx1151();
    // 200 VGPR -> waves/SIMD = min(16, 1536/200=7) = 7; waves/CU = 14.
    auto blocks = d.candidateBlocks(/*kernelVgpr=*/200, /*ldsBytes=*/0);
    ASSERT_FALSE(blocks.empty());
    // 512 threads = 16 waves/WG > 14 available -> 0 workgroups -> pruned.
    EXPECT_EQ(std::find(blocks.begin(), blocks.end(), 512u), blocks.end());
    EXPECT_EQ(std::find(blocks.begin(), blocks.end(), 1024u), blocks.end());
    // 64..448 (<=14 waves) fit.
    EXPECT_NE(std::find(blocks.begin(), blocks.end(), 64u), blocks.end());
    EXPECT_NE(std::find(blocks.begin(), blocks.end(), 128u), blocks.end());
    EXPECT_EQ(d.occupancy(512, 200, 0), 0u);
}

// A light kernel fits every block; the list is occupancy-ordered (best first).
TEST(XpuDeviceModelTests, lightKernelOrdersByOccupancy) {
    DeviceModel d = gfx1151();
    auto blocks = d.candidateBlocks(/*kernelVgpr=*/32, /*ldsBytes=*/0);
    ASSERT_GE(blocks.size(), 2u);
    // Non-increasing occupancy across the ordered list.
    for (size_t i = 1; i < blocks.size(); ++i) {
        EXPECT_GE(d.occupancy(blocks[i - 1], 32, 0),
                  d.occupancy(blocks[i], 32, 0))
            << "candidates must be ordered best-occupancy-first";
    }
}

// LDS pressure limits resident workgroups: a tile using >32 KB leaves room for
// only one workgroup per CU.
TEST(XpuDeviceModelTests, ldsBudgetLimitsOccupancy) {
    DeviceModel d = gfx1151();
    // 40 KB tile -> floor(64KB/40KB) = 1 workgroup/CU.
    unsigned occ = d.occupancy(/*block=*/256, /*kernelVgpr=*/64, /*ldsBytes=*/40000);
    unsigned wavesPerWG = 256 / 32;   // 8
    EXPECT_EQ(occ, wavesPerWG) << "one workgroup of 8 waves resident";
    // A tile bigger than the whole LDS never fits.
    EXPECT_EQ(d.occupancy(256, 64, 70000), 0u);
}

// The @Occupancy / §2 clamp caps the swept block.
TEST(XpuDeviceModelTests, clampCapsCandidateBlock) {
    DeviceModel d = gfx1151();
    auto blocks = d.candidateBlocks(/*kernelVgpr=*/32, /*ldsBytes=*/0, /*clamp=*/128);
    for (unsigned b : blocks) EXPECT_LE(b, 128u);
}

// The linear conflict-free pad reproduces the measured gfx1151 layout: a
// 64-element f16 LDS row pads by 2 (stride 66, dword-stride 33, coprime with 32).
TEST(XpuDeviceModelTests, conflictFreePadMatchesMeasuredStride) {
    EXPECT_EQ(conflictFreePad(/*rowElems=*/64, /*elemBytes=*/2), 2u);  // -> 66
    EXPECT_EQ(conflictFreePad(/*rowElems=*/128, /*elemBytes=*/2), 2u); // -> 130
    // An already-coprime row needs no pad: 33 f16 = 66 bytes = 16.5 dwords...
    // use a dword-aligned odd-stride row: 31 f32 = 124 bytes = 31 dwords (coprime).
    EXPECT_EQ(conflictFreePad(/*rowElems=*/31, /*elemBytes=*/4), 0u);
    // f32, 32-element row = 32 dwords (gcd 32) -> pad 1 -> 33 dwords (coprime).
    EXPECT_EQ(conflictFreePad(/*rowElems=*/32, /*elemBytes=*/4), 1u);
}
