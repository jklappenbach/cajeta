//
// Launch one kernel of a compiled module on the cpu backend, in process, over
// raw host buffers: lower it, register its launcher thunk through the real
// registration ctor, and run it with CpuDriver. The shape XpuCpuDriverTests
// built by hand, factored out for tests that compare the cpu backend against
// another answer (the reference interpreter).
//
// Each kernel registers under its own name, and the cpu registry is keyed by
// name, so tests give their kernels distinct names.
//
#pragma once

#include "cajeta/compile/CajetaModule.h"
#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/xpu/cpu/CpuBackend.h"
#include "cajeta/xpu/cpu/CpuDriver.h"
#include "cajeta/xpu/cpu/CpuRegistration.h"

#include "jit/CoffSafeJit.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/Mangling.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorSymbolDef.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

extern "C" void __cajeta_xpu_register_cpu_kernel(const char* name, void* fn);
extern "C" void __cajeta_xpu_set_cpu_wave(int32_t lanes);
extern "C" void __cajeta_xpu_register_kernel_manifest(const char* kernelName,
                                                      int32_t backend,
                                                      const char* arch,
                                                      const void* json,
                                                      uint64_t len);

namespace cajeta_test {

inline cajeta::MethodPtr findKernel(const cajeta::CajetaModulePtr& module,
                                    const std::string& cls,
                                    const std::string& name) {
    auto it = module->getStructures().find(cls);
    if (it == module->getStructures().end() || !it->second) return nullptr;
    for (auto& [k, m] : it->second->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

// A registered cpu kernel. Keep it alive while launching: the registered
// thunk is code in its JIT.
class CpuKernel {
public:
    static std::unique_ptr<CpuKernel> load(const cajeta::MethodPtr& kernel,
                                           std::string& failure) {
        auto self = std::unique_ptr<CpuKernel>(new CpuKernel());
        self->name = kernel->getName();
        auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
        if (!tm) { failure = "host target not registered"; return nullptr; }
        auto ctx = std::make_unique<llvm::LLVMContext>();
        auto host = std::make_unique<llvm::Module>("xpu_cpu_harness", *ctx);
        cajeta::xpu::cpu::configureHostModule(*host, *tm);
        std::vector<cajeta::MethodPtr> kernels{kernel};
        if (cajeta::xpu::cpu::emitKernelRegistration(kernels, *host) != 1) {
            failure = "the cpu backend did not lower " + self->name;
            return nullptr;
        }
        auto jitOrErr = cajeta::test::makeCoffSafeJit();
        if (!jitOrErr) { failure = llvm::toString(jitOrErr.takeError()); return nullptr; }
        self->jit = std::move(*jitOrErr);
        auto& JD = self->jit->getMainJITDylib();
        // The registration ctor calls these runtime functions, which live in the
        // test binary but are not dynamically exported (XpuCpuDriverTests says why
        // they are mapped here and not in a shared bridge table).
        llvm::orc::MangleAndInterner mangle(self->jit->getExecutionSession(),
                                            self->jit->getDataLayout());
        llvm::orc::SymbolMap syms;
        auto bind = [&](const char* n, void* p) {
            syms[mangle(n)] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(p),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
        };
        bind("__cajeta_xpu_register_cpu_kernel", (void*) &__cajeta_xpu_register_cpu_kernel);
        bind("__cajeta_xpu_register_kernel_manifest",
             (void*) &__cajeta_xpu_register_kernel_manifest);
        bind("__cajeta_xpu_set_cpu_wave", (void*) &__cajeta_xpu_set_cpu_wave);
        if (auto err = JD.define(llvm::orc::absoluteSymbols(std::move(syms)))) {
            failure = llvm::toString(std::move(err));
            return nullptr;
        }
        if (auto err = self->jit->addIRModule(
                llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)))) {
            failure = llvm::toString(std::move(err));
            return nullptr;
        }
        if (auto err = self->jit->initialize(JD)) {
            failure = llvm::toString(std::move(err));
            return nullptr;
        }
        return self;
    }

    // argv[i] points at argument i: a buffer argument is a pointer to the
    // data pointer, a scalar a pointer to its value.
    bool launch(void** argv, const uint32_t grid[3], const uint32_t block[3]) {
        cajeta::xpu::cpu::CpuDriver d;
        return d.launch(name.c_str(), argv, grid[0], block[0], grid[1], block[1],
                        grid[2], block[2]);
    }

private:
    CpuKernel() = default;
    std::string name;
    std::unique_ptr<llvm::orc::LLJIT> jit;
};

} // namespace cajeta_test
