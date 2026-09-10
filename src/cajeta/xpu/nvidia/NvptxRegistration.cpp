// NVPTX kernel registration pass — see header.

#include "NvptxRegistration.h"
#include "NvptxBackend.h"
#include "cajeta/xpu/core/KernelManifest.h"
#include "cajeta/xpu/core/XpuKernelAttr.h"
#include <optional>
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
                               const std::string& arch,
                               std::vector<KernelManifest>* manifests) {
        if (kernels.empty()) return 0;

        // One TargetMachine for every kernel: it is arch-, not kernel-, specific.
        auto tm = createNvptxTargetMachine(arch);
        if (!tm) return 0;

        llvm::LLVMContext& ctx = hostModule.getContext();
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* voidTy = llvm::Type::getVoidTy(ctx);
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::IRBuilder<> b(ctx);

        // (i8* name, i8* image, i64 len, i32 backend): backend-tagged so a
        // multi-backend build keeps one image per backend, not last-writer-wins.
        llvm::FunctionType* regTy =
            llvm::FunctionType::get(voidTy, {ptrTy, ptrTy, i64Ty, i32Ty}, false);
        llvm::FunctionCallee regFn =
            hostModule.getOrInsertFunction("__cajeta_xpu_register_module_be", regTy);

        // (i8* name, i32 count, i8* kind, i32* byteSize): without it the CUDA launch
        // path cannot translate Texture2D/Image2D args or copy bindless arrays.
        llvm::FunctionType* kpTy = llvm::FunctionType::get(
            voidTy, {ptrTy, i32Ty, ptrTy, ptrTy}, false);
        llvm::FunctionCallee kpFn = hostModule.getOrInsertFunction(
            "__cajeta_xpu_register_kernel_params", kpTy);

        // (i8* name, i8* ptx, i64 len, i32 shape, i8* raygen, i8* prog1..3): the
        // OptiX program PTX, for an AS whose launch is an optixLaunch pipeline.
        llvm::FunctionType* rqTy = llvm::FunctionType::get(
            voidTy, {ptrTy, ptrTy, i64Ty, i32Ty, ptrTy, ptrTy, ptrTy, ptrTy}, false);
        llvm::FunctionCallee rqFn = hostModule.getOrInsertFunction(
            "__cajeta_xpu_register_optix_rayquery", rqTy);

        int emitted = 0;
        for (auto& method : kernels) {
            if (!method || !isKernel(*method)) continue;
            const std::string entryName = method->getName();

            // The device lowerer has its own context; the host module waits for bytes.
            llvm::LLVMContext devCtx;
            llvm::Module devMod("xpu.dev." + entryName, devCtx);
            configureDeviceModule(devMod, *tm);
            llvm::Function* kfn = nullptr;
            try {
                kfn = lowerKernel(method, devMod);
            } catch (cajeta::Exception& ex) {
                // A contradicted @Access declaration is a compile error, not a skip.
                if (ex.getErrorId() == "CAJETA_ERROR_XPU_ACCESS_CONTRADICTED"
                        || ex.getErrorId() == "CAJETA_ERROR_XPU_ACCESS_UNKNOWN") throw;
                // No device code for this backend; say so, or a launch fails at run time.
                fprintf(stderr,
                        "cajeta: note: [xpu-kernel-skipped] %s: no nvptx device "
                        "code — %s\n",
                        entryName.c_str(), ex.getMessage().c_str());
                continue;
            }
            if (!kfn) continue;
            // Access modes come off the lowered IR, BEFORE codegen transforms it.
            KernelAccessSummary access = classifyKernelAccess(*kfn, method);

            std::string ptx = emitPtx(devMod, *tm);
            if (ptx.empty()) continue;
            std::string ptxasLog;
            std::vector<uint8_t> cubin = assembleCubin(ptx, arch, &ptxasLog);
            if (cubin.empty()) continue;  // ptxas missing or errored

            // Hash over the cubin that registers; occupancy needs an arch row sm_* lacks.
            KernelManifest manifest;
            manifest.kernel = qualifiedKernelName(method);
            manifest.target = "nvptx/" + arch;
            manifest.codeHash = sha256Hex(cubin.data(), cubin.size());
            manifest.compilerVersion = compilerVersionString();
            manifest.xpuAbiVersion = CAJETA_XPU_ABI_VERSION;
            manifest.waveWidth = 32;
            for (const PtxasKernelStats& st : parsePtxasVerbose(ptxasLog)) {
                if (st.name != entryName) continue;
                manifest.vgpr = st.registers;
                manifest.ldsStaticBytes = st.smemBytes;
                manifest.spillBytes = st.stackBytes;
            }
            {
                std::optional<unsigned> pinned;
                if (auto attr = XpuKernelAttr::from(*method))
                    if (attr->maxThreads()) pinned = *attr->maxThreads();
                fillOccupancy(manifest, arch, pinned);
            }
            applyAccess(manifest, access);
            warnIfSpilling(manifest);

            llvm::Constant* dataInit = llvm::ConstantDataArray::get(
                ctx, llvm::ArrayRef<uint8_t>(cubin.data(), cubin.size()));
            auto* cubinGV = new llvm::GlobalVariable(
                hostModule, dataInit->getType(), /*isConstant=*/true,
                llvm::GlobalValue::PrivateLinkage, dataInit,
                "xpu.cubin." + entryName);
            cubinGV->setAlignment(llvm::MaybeAlign(8));

            llvm::FunctionType* ctorTy = llvm::FunctionType::get(voidTy, false);
            llvm::Function* ctor = llvm::Function::Create(
                ctorTy, llvm::GlobalValue::InternalLinkage,
                "__cajeta_xpu_reg_ctor." + entryName, hostModule);
            llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx, "entry", ctor);
            b.SetInsertPoint(bb);
            llvm::Value* nameStr =
                b.CreateGlobalString(entryName, "xpu.kname." + entryName);
            b.CreateCall(regFn, {nameStr, cubinGV,
                                 llvm::ConstantInt::get(i64Ty, cubin.size()),
                                 llvm::ConstantInt::get(i32Ty, 0)});  // CAJ_XPU_CUDA

            // The parameter kinds the CUDA launch path translates against.
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

            // The program PTX goes in a SEPARATE module: ptxas rejects `_optix_*` asm.
            // Only the count and nearest-hit shapes register, the rest stay software.
            OptixRqShape shape = classifyRayQueryShape(method);
            if (shape != OptixRqShape::Unsupported) {
                try {
                    llvm::LLVMContext oCtx;
                    llvm::Module oMod("xpu.optix." + entryName, oCtx);
                    configureDeviceModule(oMod, *tm);
                    std::string raygen;
                    switch (shape) {
                        case OptixRqShape::NearestTri:
                            raygen = emitOptixNearestModule(method, oMod); break;
                        case OptixRqShape::BaryCandidate:
                            raygen = emitOptixBaryModule(method, oMod); break;
                        case OptixRqShape::CommittedTri:
                            raygen = emitOptixCommittedTriModule(method, oMod); break;
                        default:
                            raygen = emitOptixCountModule(method, oMod); break;
                    }
                    std::string optixPtx = emitPtx(oMod, *tm);
                    if (!optixPtx.empty()) {
                        llvm::Constant* pInit = llvm::ConstantDataArray::getString(
                            ctx, optixPtx, /*AddNull=*/true);
                        auto* ptxGV = new llvm::GlobalVariable(
                            hostModule, pInit->getType(), /*isConstant=*/true,
                            llvm::GlobalValue::PrivateLinkage, pInit,
                            "xpu.optixptx." + entryName);
                        ptxGV->setAlignment(llvm::MaybeAlign(1));
                        // Program slots as __cajeta_xpu_register_optix_rayquery reads them.
                        llvm::Value* rg = b.CreateGlobalString(raygen,
                            "xpu.orgn." + entryName);
                        llvm::Value *p1, *p2, *p3;
                        if (shape == OptixRqShape::NearestTri ||
                            shape == OptixRqShape::CommittedTri) {
                            p1 = b.CreateGlobalString("__closesthit__" + entryName, "xpu.och." + entryName);
                            p2 = b.CreateGlobalString("__miss__" + entryName, "xpu.oms." + entryName);
                            p3 = b.CreateGlobalString("", "xpu.op3." + entryName);
                        } else if (shape == OptixRqShape::BaryCandidate) {
                            p1 = b.CreateGlobalString("__anyhit__" + entryName, "xpu.oah." + entryName);
                            p2 = b.CreateGlobalString("__miss__" + entryName, "xpu.oms." + entryName);
                            p3 = b.CreateGlobalString("", "xpu.op3." + entryName);
                        } else {
                            p1 = b.CreateGlobalString("__intersection__" + entryName, "xpu.ois." + entryName);
                            p2 = b.CreateGlobalString("__anyhit__" + entryName, "xpu.oah." + entryName);
                            p3 = b.CreateGlobalString("__miss__" + entryName, "xpu.oms." + entryName);
                        }
                        b.CreateCall(rqFn, {nameStr, ptxGV,
                            llvm::ConstantInt::get(i64Ty, optixPtx.size()),
                            llvm::ConstantInt::get(i32Ty, (int32_t) shape),
                            rg, p1, p2, p3});
                    }
                } catch (cajeta::Exception&) {
                }
            }
            emitManifestRegistration(hostModule, b, nameStr, /*CAJ_XPU_CUDA=*/0,
                                     arch, manifest);
            b.CreateRetVoid();

            llvm::appendToGlobalCtors(hostModule, ctor, /*priority=*/65535);
            if (manifests) manifests->push_back(manifest);
            ++emitted;
        }
        return emitted;
    }

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
