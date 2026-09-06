//
// AMDGPU kernel registration pass — see header.
//
// Structurally identical to NvptxRegistration: for each @Kernel it lowers a
// device module, assembles a binary (hsaco here, cubin there), embeds the
// bytes as a private host-module constant, and appends an llvm.global_ctors
// entry calling the backend-neutral __cajeta_xpu_register_module(entryName,
// bytes, len). The runtime keys modules by entry name, so the same launch
// path resolves an AMD kernel exactly as it does an NVIDIA one — only the
// binary format behind it changed.
//

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

        // `arch` may be a comma-separated list ("gfx1100,gfx1151") → a multi-arch
        // bundle. One AMDGPU TargetMachine (from the first arch; the datalayout is
        // arch-neutral) configures the device modules; assembleHsacoBundle builds
        // a per-arch hsaco for each and bundles them.
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

        // void __cajeta_xpu_register_module(i8* name, i8* image, i64 len)
        llvm::FunctionType* regTy =
            llvm::FunctionType::get(voidTy, {ptrTy, ptrTy, i64Ty, i32Ty}, false);
        llvm::FunctionCallee regFn =
            hostModule.getOrInsertFunction("__cajeta_xpu_register_module_be", regTy);

        // void __cajeta_xpu_register_kernel_params(i8* name, i32 count,
        //                                          i8* kind, i32* byteSize)
        // The HIP launch path reads this to find Texture2D params (Item 8 Stage
        // C): a texture arg is translated into a hipTextureObject at launch.
        llvm::FunctionType* kpTy = llvm::FunctionType::get(
            voidTy, {ptrTy, i32Ty, ptrTy, ptrTy}, false);
        llvm::FunctionCallee kpFn = hostModule.getOrInsertFunction(
            "__cajeta_xpu_register_kernel_params", kpTy);


        int emitted = 0;
        for (auto& method : kernels) {
            if (!method || !isKernel(*method)) continue;
            const std::string entryName = method->getName();

            // Lower this kernel into a fresh device module + assemble an hsaco.
            // The device lowerer builds types in its own context, so this never
            // touches the host module until we have bytes.
            llvm::LLVMContext devCtx;
            llvm::Module devMod("xpu.dev." + entryName, devCtx);
            configureDeviceModule(devMod, *tm);
            // The single lowered IR is codegen'd for every arch in the bundle;
            // record the full list so per-subtarget feature gates (the direct
            // global->LDS load) stay conservative across all of them, not just
            // archList[0]. See AmdgpuKernelLowering::bundleHasVmemToLds.
            devMod.addModuleFlag(llvm::Module::Warning, "cajeta.amdgpu.archlist",
                                 llvm::MDString::get(devCtx, arch));
            llvm::Function* kfn = nullptr;
            try {
                kfn = lowerKernel(method, devMod);
            } catch (cajeta::Exception& ex) {
                // Unsupported construct (XPU-N01) — this kernel gets NO device
                // code for this backend; a launch that lands here at run time
                // fails with "no registered kernel". Say so at build time —
                // the silent skip cost a real debugging session (U12).
                fprintf(stderr,
                        "cajeta: note: [xpu-kernel-skipped] %s: no %s device "
                        "code — %s\n",
                        entryName.c_str(), "amdgpu", ex.getMessage().c_str());
                continue;
            }
            if (!kfn) continue;

            // kernel-occupancy-autotune §2: pin the real launch workgroup size so
            // the backend budgets registers for the true (small) occupancy.
            // Keyed by simple kernel name (= entryName, the launch receiver).
            if (auto it = maxThreads.find(entryName); it != maxThreads.end()) {
                setKernelWorkgroupSize(kfn, it->second);
            }

            std::vector<ArchHsaco> perArch = assembleHsacoPerArch(devMod, archList);
            if (perArch.empty()) continue;  // lld missing or a per-arch codegen error
            std::vector<uint8_t> hsaco = bundleHsacos(perArch);
            if (hsaco.empty()) continue;    // bundler missing or errored

            // xpu-tile-manifest §2, §3: one manifest per (kernel, arch), hashed
            // over and read from the very code object that registers below. A
            // block is pinned by an @Occupancy clamp or by one constant launch
            // block (the size the backend budgeted registers for); otherwise the
            // picker's feasible sizes are recorded (§3.3).
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
                    warnIfSpilling(m);
                    kernelManifests.push_back(std::move(m));
                }
            }

            // Embed the hsaco as a private host-module constant.
            llvm::Constant* dataInit = llvm::ConstantDataArray::get(
                ctx, llvm::ArrayRef<uint8_t>(hsaco.data(), hsaco.size()));
            auto* hsacoGV = new llvm::GlobalVariable(
                hostModule, dataInit->getType(), /*isConstant=*/true,
                llvm::GlobalValue::PrivateLinkage, dataInit,
                "xpu.hsaco." + entryName);
            hsacoGV->setAlignment(llvm::MaybeAlign(8));

            // ctor: __cajeta_xpu_register_module(entryName, hsacoGV, len)
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

            // Per-kernel parameter kinds (scalar/buffer/texture/sampler) so the
            // HIP launch path can translate Texture2D args into texture objects.
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

            // The manifests ride the same ctor, so `k.manifest()` reads them
            // from the artifact at run time (§12.1) — one per arch.
            for (size_t i = 0; i < kernelManifests.size(); ++i)
                emitManifestRegistration(hostModule, b, nameStr, /*CAJ_XPU_HIP=*/1,
                                         perArch[i].arch, kernelManifests[i]);
            b.CreateRetVoid();

            // Run at module-init time (LLJIT: jit->initialize; native: startup).
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
