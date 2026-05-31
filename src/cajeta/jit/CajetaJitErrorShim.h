//
// RTTI-free shim around the handful of llvm::Error consumers used by the
// in-process JIT host (src/cajeta/jit/CajetaJitHost.cpp).
//
// Rationale (same as test/jit/JitErrorShim.*): llvm::toString / cantFail /
// consumeError expand inline to code that instantiates the C++ typeinfo for
// llvm::ErrorInfo<...>. When the host LLVM is built -fno-rtti (ROCm / many
// upstream packages), that typeinfo dangles a reference to
// `typeinfo for llvm::ErrorInfoBase`, which the RTTI-less LLVM omits ->
// undefined reference at link. Isolating the calls in this one TU lets the
// build compile JUST this file -fno-rtti (see src/CMakeLists.txt) so no such
// typeinfo is emitted, while the rest of cajeta keeps RTTI.
//
#pragma once

#include <string>

#include "llvm/Support/Error.h"

namespace cajeta::jit {

    // Thin forwarders to llvm::consumeError / llvm::toString / llvm::cantFail.
    // Each takes the (move-only) llvm::Error by value so callers can pass an
    // rvalue from takeError() / a function returning Error directly.
    void consumeError(llvm::Error err);
    std::string toString(llvm::Error err);
    void cantFail(llvm::Error err);

} // namespace cajeta::jit
