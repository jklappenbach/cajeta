// Vulkan/SPIR-V kernel registration pass — see header. Structurally identical
// to Nvptx/AmdgpuRegistration; only the binary format differs.

#include "VulkanRegistration.h"
#include "cajeta/xpu/core/KernelManifest.h"
#include <optional>
#include "SpirvBackend.h"
#include "SpirvKernelLowering.h"

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

#include <optional>
#include <cstdio>

namespace cajeta {
namespace xpu {
namespace vulkan {

    int emitKernelRegistration(const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& arch,
                               std::vector<KernelManifest>* manifests) {
        if (kernels.empty()) return 0;

        // A FRESH TargetMachine per kernel: the SPIR-V backend keeps codegen
        // state on it, and reusing one across kernels corrupts that state.
        if (!createSpirvTargetMachine(arch)) return 0;

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
        llvm::FunctionType* kpTy = llvm::FunctionType::get(
            voidTy, {ptrTy, i32Ty, ptrTy, ptrTy}, false);
        llvm::FunctionCallee kpFn = hostModule.getOrInsertFunction(
            "__cajeta_xpu_register_kernel_params", kpTy);

        // Emits one SPIR-V variant of `method`; `registerKparams` is true only
        // for the primary variant, since the variants share that metadata.
        auto emitVariant = [&](const MethodPtr& method, const std::string& regName,
                               bool software, bool registerKparams) -> bool {
            auto tm = createSpirvTargetMachine(arch);
            if (!tm) return false;
            llvm::LLVMContext devCtx;
            llvm::Module devMod("xpu.dev." + regName, devCtx);
            configureDeviceModule(devMod, *tm);
            llvm::Function* kfn = nullptr;
            try {
                kfn = lowerKernel(method, devMod, software, regName);
            } catch (cajeta::Exception& ex) {
                // A contradicted @Access is the author's error, never a skip.
                if (ex.getErrorId() == "CAJETA_ERROR_XPU_ACCESS_CONTRADICTED"
                        || ex.getErrorId() == "CAJETA_ERROR_XPU_ACCESS_UNKNOWN") throw;
                // XPU-N01: no device code, so a launch would find no kernel.
                fprintf(stderr,
                        "cajeta: note: [xpu-kernel-skipped] %s: no vulkan device "
                        "code — %s\n",
                        regName.c_str(), ex.getMessage().c_str());
                return false;
            }
            if (!kfn) return false;
            // Access modes come off the lowered IR, before SPIR-V codegen.
            KernelAccessSummary access = classifyKernelAccess(*kfn, method);

            std::vector<uint8_t> spirv = emitSpirv(devMod, *tm);
            if (spirv.empty()) return false;  // codegen error (logged)

            // Pipeline statistics are a driver fact, so the footprint is ABSENT.
            std::optional<KernelManifest> manifest;
            if (registerKparams) {
                KernelManifest m;
                m.kernel = qualifiedKernelName(method);
                m.target = "spirv/" + arch;
                m.codeHash = sha256Hex(spirv.data(), spirv.size());
                m.compilerVersion = compilerVersionString();
                m.xpuAbiVersion = CAJETA_XPU_ABI_VERSION;
                applyAccess(m, access);
                manifest = std::move(m);
            }

            llvm::Constant* dataInit = llvm::ConstantDataArray::get(
                ctx, llvm::ArrayRef<uint8_t>(spirv.data(), spirv.size()));
            auto* spvGV = new llvm::GlobalVariable(
                hostModule, dataInit->getType(), /*isConstant=*/true,
                llvm::GlobalValue::PrivateLinkage, dataInit,
                "xpu.spirv." + regName);
            spvGV->setAlignment(llvm::MaybeAlign(8));

            llvm::FunctionType* ctorTy = llvm::FunctionType::get(voidTy, false);
            llvm::Function* ctor = llvm::Function::Create(
                ctorTy, llvm::GlobalValue::InternalLinkage,
                "__cajeta_xpu_reg_ctor." + regName, hostModule);
            llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx, "entry", ctor);
            b.SetInsertPoint(bb);
            llvm::Value* nameStr =
                b.CreateGlobalString(regName, "xpu.kname." + regName);
            b.CreateCall(regFn, {nameStr, spvGV,
                                 llvm::ConstantInt::get(i64Ty, spirv.size()),
                                 llvm::ConstantInt::get(i32Ty, 2)});  // CAJ_XPU_VULKAN

            // The runtime binds buffers directly, scalars as 1-element SSBOs.
            if (registerKparams) {
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
                        "xpu.kpkind." + regName);
                    llvm::Constant* szInit = llvm::ConstantDataArray::get(
                        ctx, llvm::ArrayRef<uint32_t>(sizes.data(), sizes.size()));
                    auto* szGV = new llvm::GlobalVariable(
                        hostModule, szInit->getType(), /*isConstant=*/true,
                        llvm::GlobalValue::PrivateLinkage, szInit,
                        "xpu.kpsz." + regName);
                    b.CreateCall(kpFn, {nameStr,
                                        llvm::ConstantInt::get(
                                            i32Ty, (uint32_t) info.size()),
                                        kindGV, szGV});
                }
            }
            if (manifest)
                emitManifestRegistration(hostModule, b, nameStr, /*CAJ_XPU_VULKAN=*/2,
                                         arch, *manifest);
            b.CreateRetVoid();

            llvm::appendToGlobalCtors(hostModule, ctor, /*priority=*/65535);
            if (manifest && manifests) manifests->push_back(*manifest);
            return true;
        };

        // A kernel uses ray query iff it has an AccelerationStructure parameter.
        auto usesRayQuery = [&](const MethodPtr& method) -> bool {
            for (auto& pi :
                 collectKernelParamInfo(method, ctx, hostModule.getDataLayout()))
                if (pi.kind == KernelParamInfo::AccelStruct) return true;
            return false;
        };

        int emitted = 0;
        for (auto& method : kernels) {
            if (!method || !isKernel(*method)) continue;
            const std::string entryName = method->getName();
            if (!emitVariant(method, entryName, /*software=*/false,
                             /*registerKparams=*/true))
                continue;
            ++emitted;
            // A ray-query kernel also gets a "<name>$sw" software-BVH variant.
            if (usesRayQuery(method))
                emitVariant(method, entryName + "$sw", /*software=*/true,
                            /*registerKparams=*/false);
        }
        return emitted;
    }

    // --- Graphics shader registration (cajeta-gfx §4) ----------------------

    namespace {
        /// The SPIR-V ShaderStage for a method's stage annotation, or nullopt.
        std::optional<ShaderStage> stageOf(const Method& m) {
            if (isVertex(m))      return ShaderStage::Vertex;
            if (isFragment(m))    return ShaderStage::Fragment;
            if (isGeometry(m))    return ShaderStage::Geometry;
            if (isTessControl(m)) return ShaderStage::TessControl;
            if (isTessEval(m))    return ShaderStage::TessEval;
            if (isMesh(m))        return ShaderStage::Mesh;
            if (isTask(m))        return ShaderStage::Task;
            return std::nullopt;
        }
    } // namespace

    // A graphics stage binds inputs as interface variables, so no kparams.
    int emitGraphicsRegistration(const std::vector<MethodPtr>& shaders,
                                 llvm::Module& hostModule,
                                 const std::string& arch) {
        if (shaders.empty()) return 0;

        llvm::LLVMContext& ctx = hostModule.getContext();
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* voidTy = llvm::Type::getVoidTy(ctx);
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::IRBuilder<> b(ctx);

        // void __cajeta_xpu_register_module_be(name, image, len, backend)
        llvm::FunctionType* regTy =
            llvm::FunctionType::get(voidTy, {ptrTy, ptrTy, i64Ty, i32Ty}, false);
        llvm::FunctionCallee regFn =
            hostModule.getOrInsertFunction("__cajeta_xpu_register_module_be", regTy);

        int emitted = 0;
        for (auto& method : shaders) {
            if (!method || !isGraphicsShader(*method)) continue;
            if (hasShaderStageConflict(*method)) continue;  // ill-formed; skip
            auto stage = stageOf(*method);
            if (!stage) continue;

            // A FRESH per-stage TargetMachine, for the reason the kernel path has.
            auto tm = createSpirvTargetMachineForStage(*stage, arch);
            if (!tm) continue;  // this stage's target env isn't in this build

            const std::string regName = method->getName();
            llvm::LLVMContext devCtx;
            llvm::Module devMod("xpu.gfx." + regName, devCtx);
            configureDeviceModuleForStage(devMod, *tm, *stage, arch);
            llvm::Function* sfn = nullptr;
            try {
                sfn = lowerGraphicsShader(method, devMod, *stage, regName);
            } catch (cajeta::Exception&) {
                // XPU-N01 leaves this stage unregistered, never fatal.
                continue;
            }
            if (!sfn) continue;

            std::vector<uint8_t> spirv = emitSpirv(devMod, *tm);
            if (spirv.empty()) continue;  // codegen error (logged)

            llvm::Constant* dataInit = llvm::ConstantDataArray::get(
                ctx, llvm::ArrayRef<uint8_t>(spirv.data(), spirv.size()));
            auto* spvGV = new llvm::GlobalVariable(
                hostModule, dataInit->getType(), /*isConstant=*/true,
                llvm::GlobalValue::PrivateLinkage, dataInit,
                "xpu.spirv." + regName);
            spvGV->setAlignment(llvm::MaybeAlign(8));

            llvm::FunctionType* ctorTy = llvm::FunctionType::get(voidTy, false);
            llvm::Function* ctor = llvm::Function::Create(
                ctorTy, llvm::GlobalValue::InternalLinkage,
                "__cajeta_xpu_reg_ctor." + regName, hostModule);
            llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx, "entry", ctor);
            b.SetInsertPoint(bb);
            llvm::Value* nameStr =
                b.CreateGlobalString(regName, "xpu.kname." + regName);
            b.CreateCall(regFn, {nameStr, spvGV,
                                 llvm::ConstantInt::get(i64Ty, spirv.size()),
                                 llvm::ConstantInt::get(i32Ty, 2)});  // CAJ_XPU_VULKAN
            b.CreateRetVoid();

            llvm::appendToGlobalCtors(hostModule, ctor, /*priority=*/65535);
            ++emitted;
        }
        return emitted;
    }

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
