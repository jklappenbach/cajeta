// XPU attribute name registry: the short type names the XPU subsystem recognizes,
// so call sites key Annotatable::findAnnotation off a constant, not a string literal.

#pragma once

#include "../../type/Annotatable.h"

namespace cajeta {
namespace xpu {

    // Route every recognition and validation lookup in src/cajeta/xpu/** through
    // these, rather than hard-coding the strings.
    struct XpuAttr {
        // Function-attribute attributes.
        static constexpr const char* Kernel       = "Kernel";
        static constexpr const char* Device       = "Device";
        static constexpr const char* Host         = "Host";

        // Kernel co-attributes.
        static constexpr const char* Wave         = "Wave";          // @Wave(width = 32)
        static constexpr const char* Backend      = "Backend";       // @Backend("nvidia"), or list
        static constexpr const char* PushConstant = "PushConstant";  // Vulkan-only
        // @FastMath relaxes IEEE FP over the whole body; opt-in, since it trades precision.
        static constexpr const char* FastMath     = "FastMath";
        // @Occupancy overrides the automatic workgroup budgeting; no-op where unsupported.
        static constexpr const char* Occupancy    = "Occupancy";
        // @Access narrows a buffer parameter's mode; @Streaming makes its access non-temporal.
        static constexpr const char* Access       = "Access";
        static constexpr const char* Streaming    = "Streaming";

        // @Intrinsic — the method has NO implementation anywhere, and none is
        // written: `@Intrinsic public void mma(...);`, a declaration with a
        // semicolon. The kernel lowering intercepts the call by receiver type
        // and method name and emits the instruction itself, so inside an
        // @Kernel the missing body is never missed. From host code there is
        // nothing to run, and MethodCallExpression refuses the call by name.
        //
        // Internally the front end already has a path for "signature only,
        // no LLVM function" — the one abstract methods take — and a bodiless
        // @Intrinsic rides it. What it must NOT do is make the class read as
        // abstract: an intrinsic is not an unfilled vtable slot waiting for a
        // subclass, it is compiler-supplied, so CajetaClass::hasAbstractMethod
        // skips it and the allocation diagnostic cannot misfire on
        // CooperativeMatrix.
        //
        // AN ANNOTATION, NOT A KEYWORD, by decision. `intrinsic` as a
        // modifier was weighed (it collides with no identifier in the
        // ecosystem and would imply `final`), and rejected — Julian,
        // 2026-09-21: a keyword removes vocabulary from the developer, gains
        // nothing over the annotation, and every other kernel construction
        // (@Kernel, @Device, @Occupancy, @Access) is an annotation. Conform.
        //
        // NOT @Device, which this was first written as and which is WRONG:
        // @Device marks a method as ALSO usable on the device, not device-
        // only. GgufFile.halfBitsToF32 is @Device, has a real body, and is
        // called from host code at GgufFile.cajeta:417.
        static constexpr const char* Intrinsic    = "Intrinsic";

        // KernelArg trait marker; the structural trait check lands later.
        static constexpr const char* KernelArg    = "KernelArg";

        // Graphics shader stages, the rasterization parallel to @Kernel; each lowers to
        // a standalone per-stage SPIR-V module. TessControl/TessEval are hull/domain.
        static constexpr const char* Vertex       = "Vertex";
        static constexpr const char* Fragment     = "Fragment";
        static constexpr const char* Geometry     = "Geometry";
        static constexpr const char* TessControl  = "TessControl";
        static constexpr const char* TessEval     = "TessEval";
        static constexpr const char* Mesh         = "Mesh";
        static constexpr const char* Task         = "Task";
    };

    // Predicate helpers; the Annotatable need not be a Method (class-level ones apply too).
    inline bool isKernel(const Annotatable& a) {
        return a.findAnnotation(XpuAttr::Kernel) != nullptr;
    }
    inline bool isDevice(const Annotatable& a) {
        return a.findAnnotation(XpuAttr::Device) != nullptr;
    }
    /// A compiler intrinsic: no implementation, the lowering IS the body.
    inline bool isIntrinsic(const Annotatable& a) {
        return a.findAnnotation(XpuAttr::Intrinsic) != nullptr;
    }
    inline bool isFastMath(const Annotatable& a) {
        return a.findAnnotation(XpuAttr::FastMath) != nullptr;
    }
    inline bool isHost(const Annotatable& a) {
        // @Host is the default; it is explicit only for the @Host @Device dual-emit case.
        return a.findAnnotation(XpuAttr::Host) != nullptr;
    }

    // --- Graphics shader-stage predicates (cajeta-gfx §4.a) ----------------
    inline bool isVertex(const Annotatable& a) {
        return a.findAnnotation(XpuAttr::Vertex) != nullptr;
    }
    inline bool isFragment(const Annotatable& a) {
        return a.findAnnotation(XpuAttr::Fragment) != nullptr;
    }
    inline bool isGeometry(const Annotatable& a) {
        return a.findAnnotation(XpuAttr::Geometry) != nullptr;
    }
    inline bool isTessControl(const Annotatable& a) {
        return a.findAnnotation(XpuAttr::TessControl) != nullptr;
    }
    inline bool isTessEval(const Annotatable& a) {
        return a.findAnnotation(XpuAttr::TessEval) != nullptr;
    }
    inline bool isMesh(const Annotatable& a) {
        return a.findAnnotation(XpuAttr::Mesh) != nullptr;
    }
    inline bool isTask(const Annotatable& a) {
        return a.findAnnotation(XpuAttr::Task) != nullptr;
    }

    // The number of graphics stage annotations; a well-formed shader carries exactly one.
    inline int graphicsStageCount(const Annotatable& a) {
        return (isVertex(a) ? 1 : 0) + (isFragment(a) ? 1 : 0)
             + (isGeometry(a) ? 1 : 0) + (isTessControl(a) ? 1 : 0)
             + (isTessEval(a) ? 1 : 0) + (isMesh(a) ? 1 : 0)
             + (isTask(a) ? 1 : 0);
    }

    // True for any graphics stage annotation: the routing parallel to isKernel.
    inline bool isGraphicsShader(const Annotatable& a) {
        return graphicsStageCount(a) > 0;
    }

    // Ill-formed if more than one graphics stage, or a graphics stage with @Kernel.
    inline bool hasShaderStageConflict(const Annotatable& a) {
        int g = graphicsStageCount(a);
        return g > 1 || (g > 0 && isKernel(a));
    }

} // namespace xpu
} // namespace cajeta
