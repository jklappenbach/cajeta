//
// XPU MIR ops.
//
// The op set is intentionally small — MIR's job is to carry
// structural metadata around kernel bodies (calling conv hints,
// address-space-qualified types, launch-site records), not to be
// a replacement for LLVM IR for general computation. Per-function
// bodies still lower through the existing AST→LLVM visitor; MIR
// ops appear only where the lowering needs an explicit decision
// point that's different per backend.
//
// v1 op list (CajetaXPU.md §2 architecture diagram):
//
//   Op_Kernel             function entry; carries calling-conv hint
//   Op_AddrSpaceCast      explicit cast between qualified types
//   Op_ThreadId           leaf builtin for xpu.thread.{x,y,z}()
//   Op_WorkgroupId        leaf builtin for xpu.workgroup.{x,y,z}()
//   Op_WorkgroupDim       leaf builtin for xpu.workgroup.dim_{x,y,z}()
//   Op_BarrierWorkgroup   portable workgroup barrier
//   Op_BarrierWave        portable wave-level barrier
//   Op_LaunchKernel       host-side launch site
//   Op_StatusBufWrite     placeholder for xpu.kernel.fail(code)
//
// In step 4 these are bare data; steps 5-7 wire them into the
// lowering visitors. Each op carries enough state for the printer
// (step 4) to round-trip its kind + payload.
//

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace cajeta {
namespace xpu {
namespace mir {

    enum class OpKind : uint8_t {
        Kernel,
        AddrSpaceCast,
        ThreadId,
        WorkgroupId,
        WorkgroupDim,
        BarrierWorkgroup,
        BarrierWave,
        LaunchKernel,
        StatusBufWrite,
    };

    // One stable name per OpKind for diagnostic / printer output.
    inline const char* opKindName(OpKind k) {
        switch (k) {
            case OpKind::Kernel:           return "kernel";
            case OpKind::AddrSpaceCast:    return "addrspacecast";
            case OpKind::ThreadId:         return "thread.id";
            case OpKind::WorkgroupId:      return "workgroup.id";
            case OpKind::WorkgroupDim:     return "workgroup.dim";
            case OpKind::BarrierWorkgroup: return "barrier.workgroup";
            case OpKind::BarrierWave:      return "barrier.wave";
            case OpKind::LaunchKernel:     return "launch.kernel";
            case OpKind::StatusBufWrite:   return "status.write";
        }
        return "?";
    }

    // Axis tag for ops that pick one of x/y/z (ThreadId,
    // WorkgroupId, WorkgroupDim).
    enum class Axis : uint8_t { X = 0, Y = 1, Z = 2 };

    inline char axisLetter(Axis a) {
        switch (a) {
            case Axis::X: return 'x';
            case Axis::Y: return 'y';
            case Axis::Z: return 'z';
        }
        return '?';
    }

    // Base op. Concrete ops derive from this and carry their per-
    // kind payload. v1 keeps everything in one struct rather than
    // a class hierarchy to make the printer trivial — the discriminant
    // (kind) tells the consumer which payload fields are meaningful.
    struct XpuMirOp {
        OpKind kind;
        Axis axis = Axis::X;                  // ThreadId/WorkgroupId/WorkgroupDim
        int32_t statusCode = 0;               // StatusBufWrite
        std::string kernelName;               // LaunchKernel: target kernel name

        explicit XpuMirOp(OpKind k) : kind(k) { }
    };

    using XpuMirOpPtr = std::shared_ptr<XpuMirOp>;

} // namespace mir
} // namespace xpu
} // namespace cajeta
