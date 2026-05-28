//
// RTTI-free shim around the handful of llvm::Error consumers used by the
// JIT test helper. See JitErrorShim.cpp for the full rationale.
//
#pragma once

#include <string>

#include "llvm/Support/Error.h"

namespace cajeta::jittest {

    // Thin forwarders to llvm::consumeError / llvm::toString / llvm::cantFail.
    // Each takes the (move-only) llvm::Error by value so callers can pass an
    // rvalue from takeError() / a function returning Error directly.
    void consumeError(llvm::Error err);
    std::string toString(llvm::Error err);
    void cantFail(llvm::Error err);

} // namespace cajeta::jittest
