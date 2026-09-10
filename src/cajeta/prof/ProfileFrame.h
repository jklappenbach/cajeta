// The handle Method/Expression hold between an instrumentation probe's enter and its
// exits (cajeta-profiler §3.1). Its own header, to break a CajetaClass.h include cycle.
#pragma once

namespace llvm { class GlobalVariable; class Value; }

namespace cajeta::prof {

    // `desc` null means no probe was emitted — profiler off, class outside the selection,
    // or the insert point already terminated; exit sites skip on the frame alone.
    struct ProfileFrame {
        llvm::GlobalVariable* desc = nullptr;
        llvm::Value*          t0Slot = nullptr;
        explicit operator bool() const { return desc != nullptr; }
    };

} // namespace cajeta::prof
