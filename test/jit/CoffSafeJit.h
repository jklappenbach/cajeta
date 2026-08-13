//
// COFF-safe LLJIT construction for tests.
//
// A bare `llvm::orc::LLJITBuilder().create()` selects LLVM's default object
// linking layer. On Windows/COFF that is RuntimeDyld, whose
// IMAGE_REL_AMD64_ADDR32NB handling calls report_fatal_error() — a process-wide
// abort — when sections do not land in image order
// (specs/windows-jit-coff-reloc-spec.md). JITLink relocates image-relative
// fixups correctly, so the library routes every builder it owns through
// cajeta::jit::applyCoffJitLink(). A test that builds its own JIT with a bare
// LLJITBuilder reintroduces the abort from the test side — and since the abort
// truncates the whole process, one such site is enough to sink an entire run.
//
// Use these helpers instead of a bare LLJITBuilder. Both are no-ops off COFF
// (ELF/Mach-O keep LLVM's default layer), so the Linux path is unchanged.
//
#pragma once

#include "cajeta/jit/JitCoffLinking.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/Support/Error.h"

#include <memory>

namespace cajeta {
namespace test {

// An LLJITBuilder with the COFF JITLink fix already applied. Start from this
// when the JIT needs generators/symbols wired in before create().
inline llvm::orc::LLJITBuilder coffSafeJitBuilder() {
    llvm::orc::LLJITBuilder builder;
    ::cajeta::jit::applyCoffJitLink(builder);
    return builder;
}

// Drop-in for `llvm::orc::LLJITBuilder().create()` that is safe on COFF hosts.
inline llvm::Expected<std::unique_ptr<llvm::orc::LLJIT>> makeCoffSafeJit() {
    return coffSafeJitBuilder().create();
}

} // namespace test
} // namespace cajeta
