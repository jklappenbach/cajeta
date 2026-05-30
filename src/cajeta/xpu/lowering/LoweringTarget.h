//
// LoweringTarget — the device-backend variance surface (cajeta-amd.md §2).
//
// The kernel-body lowerer (DeviceLowerer in KernelLowering.cpp) walks the
// @Kernel AST and emits device LLVM IR that is ~90% target-neutral: buffer
// GEPs in addrspace(1), shared globals in addrspace(3), the full int/float
// operator set, control flow, casts. Only a handful of decisions actually
// differ between NVIDIA and AMD — and THIS interface is exactly that list.
//
// It was extracted empirically by threading a second backend (AMDGPU)
// through the originally NVIDIA-only lowerer: the methods that ended up here
// ARE the measured NVIDIA∩AMD variance, not a guess. Anything NOT on this
// vtable stayed shared.
//
// Coordinate reads return an i32 value. globalId has a shared default
// (workgroupId*workgroupDim + threadId) since that identity holds on both
// backends; only the three leaf reads + barrier + kernel decoration fork.
//

#pragma once

namespace llvm {
    class Value;
    class Module;
    class Function;
    class IRBuilderBase;
}

namespace cajeta {
namespace xpu {

    class LoweringTarget {
    public:
        virtual ~LoweringTarget() = default;

        // Lowercase backend name (diagnostics).
        virtual const char* name() const = 0;

        // Address space for entry-block allocas (the mutable scalar-slot model
        // — loop counters, reassigned locals). NVPTX: 0 (generic). AMDGPU: 5
        // (private). Getting this wrong on AMDGPU is the classic first bug
        // (cajeta-amd.md §2) — an AS-0 alloca there is invalid.
        virtual unsigned allocaAddressSpace() const = 0;

        // Leaf coordinate reads (dim 0/1/2 = x/y/z). Build into `b`'s current
        // insert point; insert any intrinsic decls into `m`.
        virtual llvm::Value* threadId(llvm::IRBuilderBase& b, llvm::Module& m,
                                      unsigned dim) = 0;     // local / workitem id
        virtual llvm::Value* workgroupId(llvm::IRBuilderBase& b, llvm::Module& m,
                                         unsigned dim) = 0;  // block / CTA id
        virtual llvm::Value* workgroupDim(llvm::IRBuilderBase& b, llvm::Module& m,
                                          unsigned dim) = 0; // block dim (ntid)

        // Global thread index. Default: workgroupId*workgroupDim + threadId,
        // which is correct on both backends. Virtual so a backend with a
        // native global-id intrinsic can override.
        virtual llvm::Value* globalId(llvm::IRBuilderBase& b, llvm::Module& m,
                                      unsigned dim);

        // Workgroup barrier (synchronize all threads in the block, with the
        // memory ordering the backend needs for LDS visibility).
        virtual void workgroupBarrier(llvm::IRBuilderBase& b,
                                      llvm::Module& m) = 0;

        // Decorate a freshly-created kernel function: calling convention +
        // any kernel-marker metadata. NVPTX: ptx_kernel CC + nvvm.annotations.
        // AMDGPU: amdgpu_kernel CC, no metadata.
        virtual void decorateKernel(llvm::Function* fn, llvm::Module& m) = 0;
    };

} // namespace xpu
} // namespace cajeta
