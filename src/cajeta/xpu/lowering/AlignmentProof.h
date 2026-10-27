// The alignment a kernel buffer access provably has, from its address
// expression: one proof for every device backend (kernel-load-alignment
// spec §2). NVPTX raises its loads and stores with it today; AMDGPU and
// Vulkan take it next (plan Units 2, 3).

#pragma once

#include <cstdint>

namespace llvm {
class Argument;
class DataLayout;
class Value;
}

namespace cajeta::xpu {

    /// The widest alignment any backend takes from a proof, in bytes
    /// (spec 2.6): a 16-byte vector access is the widest load emitted.
    constexpr uint64_t kMaxProvenAlignment = 16;

    /// The bytes `addr` is provably aligned to, given that `base`, the buffer
    /// argument the address is derived from, is aligned to `baseAlign` bytes.
    /// A product adds its operands' zeros, a sum keeps the smaller, a select or
    /// phi the smallest of its arms, a mask with a constant the larger of the
    /// two, and a loop-carried value the smaller of its start and its step.
    /// Capped at kMaxProvenAlignment; 1 when nothing is proven.
    uint64_t provenAlignment(llvm::Value* addr, const llvm::Argument* base,
                             uint64_t baseAlign, const llvm::DataLayout& dl);

}
