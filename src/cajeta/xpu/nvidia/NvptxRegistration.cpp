//
// NVPTX kernel registration pass — see header.
//

#include "NvptxRegistration.h"
#include "NvptxBackend.h"
#include "NvptxKernelLowering.h"
#include "NvptxOptixRayQuery.h"

#include "../lowering/KernelLowering.h"
#include "cajeta/method/Method.h"
#include "cajeta/xpu/core/XpuAttributes.h"
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
namespace nvidia {

    int emitKernelRegistration(const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& arch) {
        if (kernels.empty()) return 0;

        // One NVPTX TargetMachine for all kernels (it's arch-, not kernel-,
        // specific). If the nvptx64 target isn't in this LLVM build, there's
        // nothing to emit.
        auto tm = createNvptxTargetMachine(arch);
        if (!tm) return 0;

        llvm::LLVMContext& ctx = hostModule.getContext();
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* voidTy = llvm::Type::getVoidTy(ctx);
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::IRBuilder<> b(ctx);

        // void __cajeta_xpu_register_module(i8* name, i8* image, i64 len)
        llvm::FunctionType* regTy =
            llvm::FunctionType::get(voidTy, {ptrTy, ptrTy, i64Ty}, false);
        llvm::FunctionCallee regFn =
            hostModule.getOrInsertFunction("__cajeta_xpu_register_module", regTy);

        // void __cajeta_xpu_register_kernel_params(i8* name, i32 count,
        //                                          i8* kind, i32* byteSize)
        // The CUDA launch path reads this to translate Texture2D/Image2D args
        // into texture/surface objects and to copy bindless Buffer<T>[] handle
        // arrays to the device (cajeta_xpu_launch_cuda). Without it find_kparams
        // returns NULL on NVIDIA and those translations silently no-op — the gap
        // the AMD/Vulkan registration passes already close.
        llvm::FunctionType* kpTy = llvm::FunctionType::get(
            voidTy, {ptrTy, i32Ty, ptrTy, ptrTy}, false);
        llvm::FunctionCallee kpFn = hostModule.getOrInsertFunction(
            "__cajeta_xpu_register_kernel_params", kpTy);

        // void __cajeta_xpu_register_optix_rayquery(i8* name, i8* ptx, i64 len,
        //     i8* raygen, i8* is, i8* anyhit, i8* miss)
        // For a ray-query kernel against an OptiX-impl AS the launch is an
        // optixLaunch pipeline, not cuLaunchKernel; this registers the OptiX
        // program PTX (a separate module) keyed by the same kernel name. The launch
        // path picks optixLaunch when the AS impl is OptiX, the cubin otherwise.
        llvm::FunctionType* rqTy = llvm::FunctionType::get(
            voidTy, {ptrTy, ptrTy, i64Ty, ptrTy, ptrTy, ptrTy, ptrTy}, false);
        llvm::FunctionCallee rqFn = hostModule.getOrInsertFunction(
            "__cajeta_xpu_register_optix_rayquery", rqTy);

        int emitted = 0;
        for (auto& method : kernels) {
            if (!method || !isKernel(*method)) continue;
            const std::string entryName = method->getName();

            // Lower this kernel into a fresh device module + assemble a cubin.
            // The device lowerer builds types in its own context, so this never
            // touches the host module until we have bytes.
            llvm::LLVMContext devCtx;
            llvm::Module devMod("xpu.dev." + entryName, devCtx);
            configureDeviceModule(devMod, *tm);
            llvm::Function* kfn = nullptr;
            try {
                kfn = lowerKernel(method, devMod);
            } catch (cajeta::Exception&) {
                // Unsupported construct (XPU-N01) — leave this kernel to the
                // CPU-emulation path; don't fail the whole compile.
                continue;
            }
            if (!kfn) continue;

            std::string ptx = emitPtx(devMod, *tm);
            if (ptx.empty()) continue;
            std::vector<uint8_t> cubin = assembleCubin(ptx, arch);
            if (cubin.empty()) continue;  // ptxas missing or errored

            // Embed the cubin as a private host-module constant.
            llvm::Constant* dataInit = llvm::ConstantDataArray::get(
                ctx, llvm::ArrayRef<uint8_t>(cubin.data(), cubin.size()));
            auto* cubinGV = new llvm::GlobalVariable(
                hostModule, dataInit->getType(), /*isConstant=*/true,
                llvm::GlobalValue::PrivateLinkage, dataInit,
                "xpu.cubin." + entryName);
            cubinGV->setAlignment(llvm::MaybeAlign(8));

            // ctor: __cajeta_xpu_register_module(entryName, cubinGV, len)
            llvm::FunctionType* ctorTy = llvm::FunctionType::get(voidTy, false);
            llvm::Function* ctor = llvm::Function::Create(
                ctorTy, llvm::GlobalValue::InternalLinkage,
                "__cajeta_xpu_reg_ctor." + entryName, hostModule);
            llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx, "entry", ctor);
            b.SetInsertPoint(bb);
            llvm::Value* nameStr =
                b.CreateGlobalString(entryName, "xpu.kname." + entryName);
            b.CreateCall(regFn, {nameStr, cubinGV,
                                 llvm::ConstantInt::get(i64Ty, cubin.size())});

            // Per-kernel parameter kinds (scalar/buffer/texture/sampler/image/
            // buffer-array) so the CUDA launch path can translate texture/image
            // args into texture/surface objects and copy bindless arrays to the
            // device. Mirrors AmdgpuRegistration / VulkanRegistration.
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

            // OptiX ray-query program set: a ray-query kernel against an OptiX-impl
            // AS runs as an optixLaunch pipeline, not cuLaunchKernel. Emit its
            // program PTX into a SEPARATE module (ptxas rejects the `_optix_*` asm,
            // so it never goes through assembleCubin) and register it keyed by the
            // same name. Only the canonical count shape is supported — a ray-query
            // kernel of any other signature throws XPU-N04 here and simply keeps its
            // software cubin (the launch then never selects the OptiX path).
            if (nvptxKernelUsesRayQuery(method)) {
                try {
                    llvm::LLVMContext oCtx;
                    llvm::Module oMod("xpu.optix." + entryName, oCtx);
                    configureDeviceModule(oMod, *tm);
                    std::string raygen = emitOptixCountModule(method, oMod);
                    std::string optixPtx = emitPtx(oMod, *tm);
                    if (!optixPtx.empty()) {
                        // PTX text as a NUL-terminated host constant; len excludes NUL.
                        llvm::Constant* pInit = llvm::ConstantDataArray::getString(
                            ctx, optixPtx, /*AddNull=*/true);
                        auto* ptxGV = new llvm::GlobalVariable(
                            hostModule, pInit->getType(), /*isConstant=*/true,
                            llvm::GlobalValue::PrivateLinkage, pInit,
                            "xpu.optixptx." + entryName);
                        ptxGV->setAlignment(llvm::MaybeAlign(1));
                        // Program names follow emitOptixCountModule: __<stage>__<k>.
                        llvm::Value* rg = b.CreateGlobalString(raygen,
                            "xpu.orgn." + entryName);
                        llvm::Value* is = b.CreateGlobalString(
                            "__intersection__" + entryName, "xpu.ois." + entryName);
                        llvm::Value* ah = b.CreateGlobalString(
                            "__anyhit__" + entryName, "xpu.oah." + entryName);
                        llvm::Value* ms = b.CreateGlobalString(
                            "__miss__" + entryName, "xpu.oms." + entryName);
                        b.CreateCall(rqFn, {nameStr, ptxGV,
                            llvm::ConstantInt::get(i64Ty, optixPtx.size()),
                            rg, is, ah, ms});
                    }
                } catch (cajeta::Exception&) {
                    // Non-canonical ray-query signature (XPU-N04) — software cubin only.
                }
            }
            b.CreateRetVoid();

            // Run at module-init time (LLJIT: jit->initialize; native: startup).
            llvm::appendToGlobalCtors(hostModule, ctor, /*priority=*/65535);
            ++emitted;
        }
        return emitted;
    }

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
