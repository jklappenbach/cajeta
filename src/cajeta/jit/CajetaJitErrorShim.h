// RTTI-free shim around the llvm::Error consumers the in-process JIT host uses:
// toString / cantFail / consumeError instantiate typeinfo for llvm::ErrorInfo<>,
// which will not link against an -fno-rtti LLVM, so only this TU is built that way.
#pragma once

#include <string>

#include "llvm/Support/Error.h"

namespace cajeta::jit {

    // Thin forwarders to llvm::consumeError / toString / cantFail; each takes the
    // move-only llvm::Error by value so callers can pass an rvalue directly.
    void consumeError(llvm::Error err);
    std::string toString(llvm::Error err);
    void cantFail(llvm::Error err);

} // namespace cajeta::jit
