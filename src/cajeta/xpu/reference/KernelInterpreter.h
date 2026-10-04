// The kernel reference interpreter (xpu-kernel-independence spec §2.2, §7.2).
//
// Runs a @Kernel's source on the host, one work-item at a time, as the
// expected answer for every backend that lowers it. It walks the same AST the
// lowerer reads but shares none of the lowering: values carry their own width
// and signedness and follow the host compiler's arithmetic, so a lowering that
// picks the wrong extension or the wrong compare disagrees with it.
//
// Work-items, workgroups, shared memory, barriers and waves are modelled
// explicitly. A workgroup barrier and every wave collective is a rendezvous:
// each work-item runs until it reaches one, and the collective is computed
// once every live member has arrived. Scheduling is deterministic, so a run
// is reproducible.
//
// A kernel that uses a construct with no reference semantics is refused by
// name before any work-item runs (XPU-REF01). It is never run partially.
// Undefined behaviour the interpreter can see (an out-of-bounds index, a read
// of shared memory no work-item wrote, a shift past the width, a barrier some
// work-items never reach) fails the run by name (XPU-REF02).
//
// The reference semantics of each built-in are in docs/specification/xpu/
// ReferenceInterpreter.md.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cajeta {
    class Method;
    using MethodPtr = std::shared_ptr<Method>;
}

namespace cajeta {
namespace xpu {
namespace reference {

    // One kernel argument, in parameter order. A buffer is host memory the
    // interpreter reads and writes in place; its element type is the
    // parameter's (`KernelBuffer<T>`). A scalar is its value's bits, read at
    // the parameter's declared width.
    struct Arg {
        void* data = nullptr;
        uint64_t count = 0;      // buffer elements
        uint64_t bits = 0;       // scalar
        uint64_t byteCount = 0;  // buffer, when given in bytes
        bool isBuffer = false;

        static Arg buffer(void* p, uint64_t elements) {
            Arg a; a.data = p; a.count = elements; a.isBuffer = true; return a;
        }
        template <typename T>
        static Arg buffer(std::vector<T>& v) { return buffer(v.data(), v.size()); }
        // A buffer given by its size in bytes; the element count follows from
        // the parameter's element type.
        static Arg bufferBytes(void* p, uint64_t bytes) {
            Arg a; a.data = p; a.byteCount = bytes; a.isBuffer = true; return a;
        }
        static Arg scalar(uint64_t raw) { Arg a; a.bits = raw; return a; }
        static Arg f32(float f);
        static Arg f64(double d);
    };

    struct Launch {
        uint32_t grid[3] = {1, 1, 1};
        uint32_t block[3] = {1, 1, 1};
        // The wave width wave operations run at. 0 takes the kernel's
        // `@Wave(width = N)`; a kernel that uses a wave operation with neither
        // is refused, since its answer depends on the width.
        uint32_t waveWidth = 0;
        // Wall-clock seconds the run may take; 0 for no limit. A run past it
        // stops with XPU-REF03.
        double budgetSeconds = 0;
    };

    // Run `kernel` over `launch`. Throws cajeta::Exception XPU-REF01 when the
    // kernel uses something the interpreter has no semantics for (nothing has
    // run), XPU-REF02 on undefined behaviour met while running, and XPU-REF03
    // when the run outlasts the launch's budget.
    void run(const MethodPtr& kernel, const std::vector<Arg>& args,
             const Launch& launch);

    // The constructs `kernel` uses that have no reference semantics, each
    // named with its line; empty when the interpreter can run it.
    std::vector<std::string> refusals(const MethodPtr& kernel);

    // A kernel parameter as the corpus runner compares it: a buffer's
    // element type, or a scalar.
    struct ParamShape {
        std::string name;
        bool isBuffer = false;
        bool isFloat = false;
        unsigned elementBytes = 0;   // a buffer's element; 0 for a scalar
    };
    std::vector<ParamShape> paramShapes(const MethodPtr& kernel);

    // Units in the last place between two floats of one precision: 0 when
    // they are the same value (+0 and -0 included), and the maximum when
    // either is NaN and the other is not.
    uint64_t ulpDistance(float a, float b);
    uint64_t ulpDistance(double a, double b);

} // namespace reference
} // namespace xpu
} // namespace cajeta
