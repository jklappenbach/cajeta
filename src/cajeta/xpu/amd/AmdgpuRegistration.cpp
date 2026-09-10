// AMDGPU kernel registration pass — see header. Structurally identical to
// NvptxRegistration, differing only in the binary format behind the entry name.

#include "AmdgpuRegistration.h"
#include "AmdgpuBackend.h"
#include "AmdgpuKernelLowering.h"

#include "../lowering/KernelLowering.h"
#include "cajeta/method/Method.h"
#include "cajeta/xpu/core/XpuAttributes.h"
#include "cajeta/xpu/core/XpuKernelAttr.h"
#include "cajeta/xpu/core/KernelManifest.h"
#include "cajeta/error/Exception.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <cstdio>

namespace cajeta {
namespace xpu {
namespace amd {

    int emitKernelRegistration(const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& arch,
                               const KernelMaxThreads& maxThreads,
                               std::vector<KernelManifest>* manifests) {
        if (kernels.empty()) return 0;

        // `arch` may be a comma-separated list, giving a multi-arch bundle. One
        // TargetMachine configures the device modules; the datalayout is arch-neutral.
        std::vector<std::string> archList = splitArchList(arch);
        if (archList.empty()) return 0;
        auto tm = createAmdgpuTargetMachine(archList[0]);
        if (!tm) return 0;

        llvm::LLVMContext& ctx = hostModule.getContext();
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* voidTy = llvm::Type::getVoidTy(ctx);
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::IRBuilder<> b(ctx);

        llvm::FunctionType* regTy =
            llvm::FunctionType::get(voidTy, {ptrTy, ptrTy, i64Ty, i32Ty}, false);
        llvm::FunctionCallee regFn =
            hostModule.getOrInsertFunction("__cajeta_xpu_register_module_be", regTy);

        // register_kernel_params(name, count, kind, byteSize): how launch finds textures.
        llvm::FunctionType* kpTy = llvm::FunctionType::get(
            voidTy, {ptrTy, i32Ty, ptrTy, ptrTy}, false);
        llvm::FunctionCallee kpFn = hostModule.getOrInsertFunction(
            "__cajeta_xpu_register_kernel_params", kpTy);


        int emitted = 0;
        for (auto& method : kernels) {
            if (!method || !isKernel(*method)) continue;
            const std::string entryName = method->getName();

            // The device lowerer has its own context; the host module is untouched until bytes.
            llvm::LLVMContext devCtx;
            llvm::Module devMod("xpu.dev." + entryName, devCtx);
            configureDeviceModule(devMod, *tm);
            // One lowered IR is codegen'd for every arch, so record the FULL list: per-
            // subtarget feature gates must stay conservative across all of them.
            devMod.addModuleFlag(llvm::Module::Warning, "cajeta.amdgpu.archlist",
                                 llvm::MDString::get(devCtx, arch));
            llvm::Function* kfn = nullptr;
            try {
                kfn = lowerKernel(method, devMod);
            } catch (cajeta::Exception& ex) {
                // A contradicted @Access is the author's error: a compile error, never a skip.
                if (ex.getErrorId() == "CAJETA_ERROR_XPU_ACCESS_CONTRADICTED"
                        || ex.getErrorId() == "CAJETA_ERROR_XPU_ACCESS_UNKNOWN") throw;
                // Unsupported construct: this kernel gets NO device code here, and a
                // launch would fail with "no registered kernel", so say so at build time.
                fprintf(stderr,
                        "cajeta: note: [xpu-kernel-skipped] %s: no %s device "
                        "code — %s\n",
                        entryName.c_str(), "amdgpu", ex.getMessage().c_str());
                continue;
            }
            if (!kfn) continue;
            // What the lowered body does to each buffer, read off the IR before assembly.
            KernelAccessSummary access = classifyKernelAccess(*kfn, method);

            // Pin the real launch workgroup size so registers are budgeted for it.
            if (auto it = maxThreads.find(entryName); it != maxThreads.end()) {
                setKernelWorkgroupSize(kfn, it->second);
            }

            std::vector<ArchHsaco> perArch = assembleHsacoPerArch(devMod, archList);
            if (perArch.empty()) continue;  // lld missing or a per-arch codegen error
            std::vector<uint8_t> hsaco = bundleHsacos(perArch);
            if (hsaco.empty()) continue;    // bundler missing or errored

            // One manifest per (kernel, arch), hashed over the very code object that
            // registers below; an unpinned block records the picker's feasible sizes.
            std::vector<KernelManifest> kernelManifests;
            {
                std::optional<unsigned> pinned;
                if (auto attr = XpuKernelAttr::from(*method))
                    if (attr->maxThreads()) pinned = *attr->maxThreads();
                if (!pinned)
                    if (auto it = maxThreads.find(entryName); it != maxThreads.end())
                        pinned = it->second;
                for (const ArchHsaco& ah : perArch) {
                    KernelManifest m;
                    m.kernel = qualifiedKernelName(method);
                    m.target = "amdgpu/" + ah.arch;
                    m.codeHash = sha256Hex(ah.hsaco.data(), ah.hsaco.size());
                    m.compilerVersion = compilerVersionString();
                    m.xpuAbiVersion = CAJETA_XPU_ABI_VERSION;
                    for (const AmdCodeObjectFootprint& fp : readCodeObjectFootprint(ah.hsaco)) {
                        if (fp.name != entryName) continue;
                        m.vgpr = fp.vgpr;
                        m.sgpr = fp.sgpr;
                        m.spillBytes = fp.privateSegmentBytes;
                        m.ldsStaticBytes = fp.groupSegmentBytes;
                        m.waveWidth = fp.wavefrontSize;
                    }
                    fillOccupancy(m, ah.arch, pinned);
                    applyAccess(m, access);
                    warnIfSpilling(m);
                    kernelManifests.push_back(std::move(m));
                }
            }

            llvm::Constant* dataInit = llvm::ConstantDataArray::get(
                ctx, llvm::ArrayRef<uint8_t>(hsaco.data(), hsaco.size()));
            auto* hsacoGV = new llvm::GlobalVariable(
                hostModule, dataInit->getType(), /*isConstant=*/true,
                llvm::GlobalValue::PrivateLinkage, dataInit,
                "xpu.hsaco." + entryName);
            hsacoGV->setAlignment(llvm::MaybeAlign(8));

            llvm::FunctionType* ctorTy = llvm::FunctionType::get(voidTy, false);
            llvm::Function* ctor = llvm::Function::Create(
                ctorTy, llvm::GlobalValue::InternalLinkage,
                "__cajeta_xpu_reg_ctor." + entryName, hostModule);
            llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx, "entry", ctor);
            b.SetInsertPoint(bb);
            llvm::Value* nameStr =
                b.CreateGlobalString(entryName, "xpu.kname." + entryName);
            b.CreateCall(regFn, {nameStr, hsacoGV,
                                 llvm::ConstantInt::get(i64Ty, hsaco.size()),
                                 llvm::ConstantInt::get(i32Ty, 1)});  // CAJ_XPU_HIP

            // Per-kernel parameter kinds, so the HIP launch path can translate Texture2D
            // args into texture objects.
            std::vector<KernelParamInfo> info =
                collectKernelParamInfo(method, ctx, hostModule.getDataLayout());
            if (!info.empty()) {
                std::vector<uint8_t> kinds;
                std::vector<uint32_t> sizes;
                kinds.reserve(info.size());
                sizes.reserve(info.size());
                for (auto& pi : info) {
                    kinds.push_back(pi.kind);
                    sizes.push_back(pi.byteSize);
                }
                llvm::Constant* kindInit = llvm::ConstantDataArray::get(
                    ctx, llvm::ArrayRef<uint8_t>(kinds.data(), kinds.size()));
                auto* kindGV = new llvm::GlobalVariable(
                    hostModule, kindInit->getType(), /*isConstant=*/true,
                    llvm::GlobalValue::PrivateLinkage, kindInit,
                    "xpu.kpkind." + entryName);
                llvm::Constant* szInit = llvm::ConstantDataArray::get(
                    ctx, llvm::ArrayRef<uint32_t>(sizes.data(), sizes.size()));
                auto* szGV = new llvm::GlobalVariable(
                    hostModule, szInit->getType(), /*isConstant=*/true,
                    llvm::GlobalValue::PrivateLinkage, szInit,
                    "xpu.kpsz." + entryName);
                b.CreateCall(kpFn, {nameStr,
                                    llvm::ConstantInt::get(i32Ty,
                                                           (uint32_t) info.size()),
                                    kindGV, szGV});
            }

            for (size_t i = 0; i < kernelManifests.size(); ++i)
                emitManifestRegistration(hostModule, b, nameStr, /*CAJ_XPU_HIP=*/1,
                                         perArch[i].arch, kernelManifests[i]);
            b.CreateRetVoid();

            llvm::appendToGlobalCtors(hostModule, ctor, /*priority=*/65535);
            if (manifests)
                manifests->insert(manifests->end(), kernelManifests.begin(),
                                  kernelManifests.end());
            ++emitted;
        }
        return emitted;
    }

} // namespace amd
} // namespace xpu
} // namespace cajeta
