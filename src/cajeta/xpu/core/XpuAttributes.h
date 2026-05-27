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

        // KernelArg trait marker (v1 simulates the trait via this
        // annotation; full structural-trait check lands later).
        static constexpr const char* KernelArg    = "KernelArg";
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
    inline bool isHost(const Annotatable& a) {
        // @Host is the default — present-or-absent is the same to the
        // codegen routing. The explicit attribute is supported for
        // @Host @Device dual-emit cases (a function callable from both
        // host and device code, emitted twice).
        return a.findAnnotation(XpuAttr::Host) != nullptr;
    }

} // namespace xpu
} // namespace cajeta
