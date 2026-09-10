// XPU MIR ops. The set stays small on purpose: MIR carries structural metadata
// around kernel bodies, not general computation — bodies still lower through the
// AST visitor, and an op appears only where a backend needs its own decision.

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

    // Axis tag for the ops that pick one of x/y/z.
    enum class Axis : uint8_t { X = 0, Y = 1, Z = 2 };

    inline char axisLetter(Axis a) {
        switch (a) {
            case Axis::X: return 'x';
            case Axis::Y: return 'y';
            case Axis::Z: return 'z';
        }
        return '?';
    }

    // One struct for every op rather than a hierarchy, to keep the printer
    // trivial: `kind` tells a consumer which payload fields are meaningful.
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
