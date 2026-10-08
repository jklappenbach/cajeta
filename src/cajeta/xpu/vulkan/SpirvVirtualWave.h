// A virtual wave on Vulkan: a kernel that declares @Wave(width = W) runs on subgroups of S < W,
// each invocation carrying C = W / S logical lanes as a vector (vulkan-virtual-waves spec 3).
#pragma once

#include <string>
#include <vector>

namespace llvm {
    class Function;
    class FunctionType;
    class Module;
    class TargetMachine;
    class Type;
}

namespace cajeta {
namespace xpu {
namespace vulkan {

    inline constexpr const char* kVirtualPrefix = "__cajeta_xpu_wave_vk_";
    inline constexpr const char* kVirtualShuffle = "__cajeta_xpu_wave_vk_shuffle";
    inline constexpr const char* kVirtualReduceSumF32 = "__cajeta_xpu_wave_vk_reduce_sum_f32";
    inline constexpr const char* kVirtualReduceMaxF32 = "__cajeta_xpu_wave_vk_reduce_max_f32";
    inline constexpr const char* kVirtualReduceMinF32 = "__cajeta_xpu_wave_vk_reduce_min_f32";
    inline constexpr const char* kVirtualSegSumF32 = "__cajeta_xpu_wave_vk_segreduce_sum_f32";
    inline constexpr const char* kVirtualSegMaxF32 = "__cajeta_xpu_wave_vk_segreduce_max_f32";
    inline constexpr const char* kVirtualBallot = "__cajeta_xpu_wave_vk_ballot";
    inline constexpr const char* kVirtualIntReduce = "__cajeta_xpu_wave_vk_ireduce_";
    inline constexpr const char* kVirtualScanSum = "__cajeta_xpu_wave_vk_prefix_sum";
    inline constexpr const char* kVirtualScanProduct = "__cajeta_xpu_wave_vk_prefix_product";

    // The per-lane stub a virtual wave verb lowers to; the slot vectorizer swaps in its C-wide variant.
    llvm::Function* virtualWaveStub(llvm::Module& m, const char* name, llvm::Type* ret,
                                    const std::vector<llvm::Type*>& args);

    // Wraps the per-slot body `slotFn` in a GLCompute entry `entryName` that runs its C slots as
    // one vector. Returns null with the reason in `whyNot` when a verb was left per lane.
    llvm::Function* buildVirtualEntry(llvm::Function* slotFn, llvm::Module& m,
                                      llvm::TargetMachine& tm, const std::string& entryName,
                                      unsigned waveWidth, unsigned subgroup,
                                      std::string* whyNot);

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
