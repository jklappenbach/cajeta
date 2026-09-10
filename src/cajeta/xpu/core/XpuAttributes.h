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
