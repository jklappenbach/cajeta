//
// XPU attribute name registry.
//
// Centralizes the short type names recognized by the XPU subsystem so
// call sites don't sprinkle string literals. Recognition itself is
// free — Annotatable already captures every @-annotation on a
// declaration into an AnnotationInstance; these helpers just wrap
// findAnnotation lookups by canonical XPU attribute name.
//
// PascalCase per the project's framework-annotation convention
// (@Native, @Component, @Inject, @SuppressLint). The CajetaXPU.md spec
// reads `@kernel` / `@device` lowercase; the implementation uses
// PascalCase to match the surrounding codebase. Docs to be reconciled.
//

#pragma once

#include "../../type/Annotatable.h"

namespace cajeta {
namespace xpu {

    // Short type names for the XPU-recognized attribute set. Used as
    // the key argument to Annotatable::findAnnotation. All recognition
    // and validation lookups in src/cajeta/xpu/** should route through
    // these constants instead of hard-coding the strings.
    struct XpuAttr {
        // Function-attribute attributes per CajetaXPU.md §3.1.1.
        static constexpr const char* Kernel       = "Kernel";
        static constexpr const char* Device       = "Device";
        static constexpr const char* Host         = "Host";

        // Kernel co-attributes.
        static constexpr const char* Wave         = "Wave";          // @Wave(width = 32)
        static constexpr const char* Backend      = "Backend";       // @Backend("nvidia"), or list
        static constexpr const char* PushConstant = "PushConstant";  // Vulkan-only
        // @FastMath on a @Kernel: relax IEEE FP for the whole body — the backend
        // may fuse (FMA), reassociate, use reciprocals, and pick approximate
        // transcendentals (the LLVM fast-math flags). Opt-in (precision-trading).
        static constexpr const char* FastMath     = "FastMath";
        // @Occupancy(maxThreads=, minResident=, maxRegisters=) — the portable
        // override for resource logistics (kernel-occupancy-autotune §3). All
        // params optional + vendor-neutral; lowered per-backend, no-op where
        // unsupported. Overrides the automatic workgroup-size budgeting (§2).
        static constexpr const char* Occupancy    = "Occupancy";
        // @Autotune (optionally @Autotune(blocks = {64, 128, 256})) — opt-in marker
        // that a kernel is block-flexible (grid-stride) and may be runtime-tuned
        // over a candidate set (kernel-occupancy-autotune §4). The kernel is then
        // compiled with flat-work-group-size pinned to the LARGEST candidate so
        // every swept block size is a legal launch.
        static constexpr const char* Autotune     = "Autotune";

        // KernelArg trait marker (v1 simulates the trait via this
        // annotation; full structural-trait check lands later).
        static constexpr const char* KernelArg    = "KernelArg";

        // Graphics shader-stage attributes (cajeta-gfx §4.a) — the rasterization
        // parallel to @Kernel. A graphics method is lowered to a standalone
        // per-stage SPIR-V module (see SpirvBackend::ShaderStage). @TessControl /
        // @TessEval are the cajeta spellings of the hull / domain stages.
        static constexpr const char* Vertex       = "Vertex";
        static constexpr const char* Fragment     = "Fragment";
        static constexpr const char* Geometry     = "Geometry";
        static constexpr const char* TessControl  = "TessControl";
        static constexpr const char* TessEval     = "TessEval";
        static constexpr const char* Mesh         = "Mesh";
        static constexpr const char* Task         = "Task";
    };

    // Predicate helpers. The Annotatable need not be a Method —
    // class-level annotations work too (e.g. @Backend on a class
    // gates every kernel in it).
    inline bool isKernel(const Annotatable& a) {
        return a.findAnnotation(XpuAttr::Kernel) != nullptr;
    }
    inline bool isDevice(const Annotatable& a) {
        return a.findAnnotation(XpuAttr::Device) != nullptr;
    }
    // @FastMath kernel: relax FP (fast-math flags) for the whole body.
    inline bool isFastMath(const Annotatable& a) {
        return a.findAnnotation(XpuAttr::FastMath) != nullptr;
    }
    // @Autotune kernel: block-flexible, eligible for runtime config tuning.
    inline bool isAutotune(const Annotatable& a) {
        return a.findAnnotation(XpuAttr::Autotune) != nullptr;
    }
    inline bool isHost(const Annotatable& a) {
        // @Host is the default — present-or-absent is the same to the
        // codegen routing. The explicit attribute is supported for
        // @Host @Device dual-emit cases (a function callable from both
        // host and device code, emitted twice).
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

    // The number of graphics shader-stage annotations on a declaration. A
    // well-formed graphics shader carries exactly one.
    inline int graphicsStageCount(const Annotatable& a) {
        return (isVertex(a) ? 1 : 0) + (isFragment(a) ? 1 : 0)
             + (isGeometry(a) ? 1 : 0) + (isTessControl(a) ? 1 : 0)
             + (isTessEval(a) ? 1 : 0) + (isMesh(a) ? 1 : 0)
             + (isTask(a) ? 1 : 0);
    }

    // True when the declaration carries any graphics shader-stage annotation —
    // the routing parallel to isKernel (compute).
    inline bool isGraphicsShader(const Annotatable& a) {
        return graphicsStageCount(a) > 0;
    }

    // Stage-shape validation: a declaration is ill-formed if it carries more
    // than one graphics stage, or mixes a graphics stage with @Kernel (a method
    // is either one compute kernel or one graphics stage, never both/several).
    inline bool hasShaderStageConflict(const Annotatable& a) {
        int g = graphicsStageCount(a);
        return g > 1 || (g > 0 && isKernel(a));
    }

} // namespace xpu
} // namespace cajeta
