// XPU backend dispatch — see header.
#include "XpuTarget.h"

#include "nvidia/NvptxRegistration.h"
#include "amd/AmdgpuRegistration.h"
#include "vulkan/VulkanRegistration.h"
#include "cpu/CpuRegistration.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <sstream>

#include <string>

namespace cajeta {
namespace xpu {

    int emitKernelRegistration(Backend backend,
                               const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& arch,
                               const std::unordered_map<std::string, unsigned>&
                                   kernelMaxThreads,
                               std::vector<KernelManifest>* manifests) {
        switch (backend) {
            case Backend::Nvptx:
                return nvidia::emitKernelRegistration(kernels, hostModule, arch,
                                                      manifests);
            case Backend::Amdgpu:
                // Only AMDGPU consumes the workgroup sizes (amdgpu-flat-work-group-size).
                return amd::emitKernelRegistration(kernels, hostModule, arch,
                                                   kernelMaxThreads, manifests);
            case Backend::Spirv:
                return vulkan::emitKernelRegistration(kernels, hostModule, arch,
                                                      manifests);
            case Backend::Cpu:
                return cpu::emitKernelRegistration(kernels, hostModule, arch,
                                                   manifests);
        }
        return 0;
    }

    int emitGraphicsRegistration(Backend backend,
                                 const std::vector<MethodPtr>& shaders,
                                 llvm::Module& hostModule,
                                 const std::string& arch) {
        if (backend == Backend::Spirv)
            return vulkan::emitGraphicsRegistration(shaders, hostModule, arch);
        return 0;
    }

    void emitBackendManifest(const std::vector<Backend>& backends,
                             llvm::Module& hostModule) {
        if (backends.empty()) return;
        llvm::LLVMContext& ctx = hostModule.getContext();
        llvm::Type* voidTy = llvm::Type::getVoidTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);

        // void __cajeta_xpu_register_backend(i32 id)
        llvm::FunctionType* regTy =
            llvm::FunctionType::get(voidTy, {i32}, false);
        llvm::FunctionCallee regFn = hostModule.getOrInsertFunction(
            "__cajeta_xpu_register_backend", regTy);

        llvm::IRBuilder<> b(ctx);
        for (Backend be : backends) {
            const int id = (int) be;
            llvm::FunctionType* ctorTy = llvm::FunctionType::get(voidTy, false);
            llvm::Function* ctor = llvm::Function::Create(
                ctorTy, llvm::GlobalValue::InternalLinkage,
                std::string("__cajeta_xpu_backend_manifest_ctor.") +
                    std::to_string(id),
                &hostModule);
            llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx, "entry", ctor);
            b.SetInsertPoint(bb);
            b.CreateCall(regFn, {llvm::ConstantInt::get(i32, id)});
            b.CreateRetVoid();
            llvm::appendToGlobalCtors(hostModule, ctor, /*priority=*/65535);
        }
    }

    bool parseCalibrationSnapshot(const std::string& text, std::vector<CalibrationRecordBlob>& out,
                                  std::string& error) {
        static const std::string magic = "cajeta-calibration-snapshot-1\n";
        if (text.compare(0, magic.size(), magic) != 0) {
            error = "not a calibration snapshot (no `cajeta-calibration-snapshot-1` header)";
            return false;
        }
        size_t at = magic.size();
        while (at < text.size()) {
            size_t eol = text.find('\n', at);
            if (eol == std::string::npos) {
                error = "truncated record header at byte " + std::to_string(at);
                return false;
            }
            std::istringstream head(text.substr(at, eol - at));
            CalibrationRecordBlob r;
            size_t len = 0;
            if (!(head >> r.slot >> len >> r.kernel) || r.slot.rfind("cal-", 0) != 0
                    || r.slot.size() >= 128) {
                error = "malformed record header at byte " + std::to_string(at);
                return false;
            }
            at = eol + 1;
            if (len == 0 || at + len + 1 > text.size() || text[at + len] != '\n') {
                error = "record " + r.slot + " is shorter than its declared " + std::to_string(len)
                    + " bytes";
                return false;
            }
            r.bytes = text.substr(at, len);
            out.push_back(std::move(r));
            at += len + 1;
        }
        return true;
    }

    void emitCalibrationRegistration(const std::vector<CalibrationRecordBlob>& records,
                                     llvm::Module& hostModule) {
        if (records.empty()) return;
        llvm::LLVMContext& ctx = hostModule.getContext();
        llvm::Type* voidTy = llvm::Type::getVoidTy(ctx);
        llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
        // void __cajeta_xpu_register_calibration_record(const char* slot, const uint8_t* bytes, i64 len)
        llvm::FunctionCallee regFn = hostModule.getOrInsertFunction(
            "__cajeta_xpu_register_calibration_record",
            llvm::FunctionType::get(voidTy, {ptrTy, ptrTy, i64}, false));
        llvm::Function* ctor = llvm::Function::Create(
            llvm::FunctionType::get(voidTy, false), llvm::GlobalValue::InternalLinkage,
            "__cajeta_xpu_calibration_ctor", &hostModule);
        llvm::IRBuilder<> b(ctx);
        b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", ctor));
        for (const CalibrationRecordBlob& r : records) {
            llvm::Constant* slotInit = llvm::ConstantDataArray::getString(ctx, r.slot, true);
            auto* slotGv = new llvm::GlobalVariable(hostModule, slotInit->getType(), true,
                llvm::GlobalValue::PrivateLinkage, slotInit, "xpu.calibration.slot");
            llvm::Constant* bytesInit = llvm::ConstantDataArray::get(ctx,
                llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t*>(r.bytes.data()), r.bytes.size()));
            auto* bytesGv = new llvm::GlobalVariable(hostModule, bytesInit->getType(), true,
                llvm::GlobalValue::PrivateLinkage, bytesInit, "xpu.calibration.record");
            b.CreateCall(regFn, {slotGv, bytesGv, llvm::ConstantInt::get(i64, (int64_t) r.bytes.size())});
        }
        b.CreateRetVoid();
        llvm::appendToGlobalCtors(hostModule, ctor, /*priority=*/65535);
    }

} // namespace xpu
} // namespace cajeta
