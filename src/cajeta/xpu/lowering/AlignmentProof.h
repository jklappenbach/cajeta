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
class Instruction;
class Module;
class IRBuilderBase;
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

    /// The low bits of an integer `index` that are provably zero, by the same
    /// algebra, with no buffer base involved: the element alignment of an
    /// index into a descriptor-bound buffer (Vulkan), whose base the driver
    /// aligns. Not capped; 0 when nothing is proven.
    unsigned provenIndexTrailingZeros(llvm::Value* index, const llvm::DataLayout& dl);

    /// States `bytes` of alignment on a buffer load or store of
    /// `accessBytes` (an `align:` statement, spec 3.1). A statement of the
    /// full alignment the access can use (16, or the access width) goes on
    /// the access itself: no proof can raise it further. A smaller one rides
    /// as an `llvm.assume` align bundle on the address, so the pipeline
    /// neither drops it (it drops metadata when it retypes a load) nor lets
    /// it cap the alignment the proof finds later (spec 3.3); the backend's
    /// applyStatedAlignments puts it on the access after the pipeline.
    void stateAlignment(llvm::IRBuilderBase& b, llvm::Instruction* access,
                        uint64_t bytes, uint64_t accessBytes);

    /// Raises each load or store whose address an `align` assumption covers to
    /// at least that alignment; run after the pipeline and before a
    /// proof-based raise, so the emitted alignment is max(proven, stated).
    void applyStatedAlignments(llvm::Module& m);

    /// The kernel's alignment fault record (kernel-load-alignment Unit 5, spec
    /// 4.1): two i64 words in device global memory, zero until a checked
    /// `align:` statement is violated. Word 0 is (line << 8) | the stated
    /// bytes, written once by the first violation; word 1 its element index.
    /// The runtime reads it after a launch of a kernel that has one.
    constexpr const char* kAlignFaultGlobal = "__cajeta_xpu_align_fault";

    /// Erases the fault record when the pipeline folded every check away (the
    /// proof confirmed each statement, spec 4.3), so the launch owes no
    /// readback.
    void dropUnusedAlignFault(llvm::Module& m);

}
