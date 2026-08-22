// The handle Method/Expression hold between an instrumentation probe's enter
// and its exits (cajeta-profiler §3.1).
//
// Its own header, deliberately. Method.h needs the TYPE, and pulling
// ProfileCodegen.h in for it drags CajetaModule.h -> CajetaClass.h into a
// translation unit that CajetaClass.h itself is midway through building, which
// is a cycle. Nothing here needs more than two forward declarations.
#pragma once

namespace llvm { class GlobalVariable; class Value; }

namespace cajeta::prof {

    // `desc` null means no probe was emitted — profiler off, class outside the
    // selection (§3.8), or the insert point already terminated. Every exit site
    // skips on the frame alone and never re-decides membership.
    struct ProfileFrame {
        llvm::GlobalVariable* desc = nullptr;
        llvm::Value*          t0Slot = nullptr;
        explicit operator bool() const { return desc != nullptr; }
    };

} // namespace cajeta::prof
