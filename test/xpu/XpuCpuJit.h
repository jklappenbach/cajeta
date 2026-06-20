//
// Shared LLJIT factory for executing CPU-lowered XPU kernels in tests.
//
// A bare `llvm::orc::LLJITBuilder().create()` yields a JIT whose main dylib
// resolves nothing beyond the module itself: no process-symbol generator and
// none of the Windows symbol bridge. That is fine for kernels that need no
// external symbols, but any kernel that lowers to a libcall fails to
// materialize — transcendentals lower to the sincos/sincosf libcall, bf16
// arithmetic to compiler-rt's __truncsfbf2/__extendbfsf2. On POSIX those
// resolve via the process generator (libm/libgcc are dynamically visible); on
// Windows/MinGW they're statically linked and absent from the host PE export
// table, so the materialization fails with "Symbols not found: [ sincosf ]".
//
// makeCpuKernelJit() mirrors the dylib wiring in
// src/cajeta/jit/CajetaJitHost.cpp: a DynamicLibrarySearchGenerator for the
// current process plus the winJitSymbols() absolute-symbol table for the
// libm/libmingwex/libgcc names missing from the PE export table. Every XPU CPU
// oracle that JITs a lowered kernel should build its JIT through this so the
// host platform never changes which kernels can run.
//
#pragma once

#include "cajeta/jit/CajetaJitWinSymbols.h"

#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/Support/Error.h"

#include <memory>

namespace cajeta {
namespace xpu {
namespace test {

// Build an LLJIT ready to execute CPU-lowered XPU kernels (process symbols +
// the Windows symbol bridge). Returns the JIT or the first error encountered.
inline llvm::Expected<std::unique_ptr<llvm::orc::LLJIT>> makeCpuKernelJit() {
    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    if (!jitOrErr)
        return jitOrErr.takeError();
    auto jit = std::move(*jitOrErr);

    auto& dylib = jit->getMainJITDylib();
    auto generator = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        jit->getDataLayout().getGlobalPrefix());
    if (!generator)
        return generator.takeError();
    dylib.addGenerator(std::move(*generator));

    size_t winSymCount = 0;
    const ::cajeta::jit::JitWinSym* winSyms =
        ::cajeta::jit::winJitSymbols(&winSymCount);
    if (winSymCount) {
        auto& execSession = jit->getExecutionSession();
        llvm::orc::SymbolMap winSymMap;
        for (size_t i = 0; i < winSymCount; ++i) {
            winSymMap[execSession.intern(winSyms[i].name)] =
                llvm::orc::ExecutorSymbolDef(
                    llvm::orc::ExecutorAddr::fromPtr(winSyms[i].addr),
                    llvm::JITSymbolFlags::Exported);
        }
        if (auto err =
                dylib.define(llvm::orc::absoluteSymbols(std::move(winSymMap))))
            return std::move(err);
    }

    return jit;
}

} // namespace test
} // namespace xpu
} // namespace cajeta
