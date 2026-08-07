//
// Vulkan/SPIR-V kernel registration pass — see header.
//
// Structurally identical to Nvptx/AmdgpuRegistration: for each @Kernel it
// lowers a device module, emits the SPIR-V binary, embeds the bytes as a
// private host-module constant, and appends an llvm.global_ctors entry calling
// the backend-neutral __cajeta_xpu_register_module(entryName, bytes, len). The
// runtime keys modules by entry name, so the same launch path resolves a
// Vulkan kernel exactly as it does an NVIDIA or AMD one — only the binary
// format behind it (cubin/hsaco → SPIR-V) changed.
//

#include "VulkanRegistration.h"
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

namespace cajeta {
namespace xpu {
namespace vulkan {

    int emitKernelRegistration(const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& arch) {
        if (kernels.empty()) return 0;

        // A FRESH SPIR-V TargetMachine per kernel (created in the loop below):
        // LLVM's SPIR-V backend carries codegen state (SPIRVGlobalRegistry) on
        // the TargetMachine, and reusing one TM across multiple kernels'
        // emitSpirv corrupts it — a crash in SPIRVEmitIntrinsics on the second
        // kernel. This probe just confirms the spirv target is in this LLVM
        // build; if not, there's nothing to emit.
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
        // The Vulkan rung of the runtime dispatcher needs this to turn the
        // uniform kernelParams argv into descriptor bindings (scalars -> SSBOs,
        // buffers -> storage buffers, textures -> sampled images, samplers ->
        // VkSamplers). kind[i] = KernelParamInfo::Kind for param i.
        llvm::FunctionType* kpTy = llvm::FunctionType::get(
            voidTy, {ptrTy, i32Ty, ptrTy, ptrTy}, false);
        llvm::FunctionCallee kpFn = hostModule.getOrInsertFunction(
            "__cajeta_xpu_register_kernel_params", kpTy);

        // Emit + register one SPIR-V variant of `method` under `regName`. `software`
        // selects the SoftwareRayQuery target (the "<name>$sw" variant); a fresh
        // TargetMachine per call (see above). `registerKparams` is true only for the
        // primary variant — the param metadata is shared (the launch overrides the
        // AS bind kind per the recorded impl), so the $sw variant registers only its
        // module. Returns true on success.
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
            } catch (cajeta::Exception&) {
                // Unsupported construct (XPU-N01) — leave this kernel to the
                // host path; don't fail the whole compile.
                return false;
            }
            if (!kfn) return false;

            std::vector<uint8_t> spirv = emitSpirv(devMod, *tm);
            if (spirv.empty()) return false;  // codegen error (logged)

            // Embed the SPIR-V as a private host-module constant.
            llvm::Constant* dataInit = llvm::ConstantDataArray::get(
                ctx, llvm::ArrayRef<uint8_t>(spirv.data(), spirv.size()));
            auto* spvGV = new llvm::GlobalVariable(
                hostModule, dataInit->getType(), /*isConstant=*/true,
                llvm::GlobalValue::PrivateLinkage, dataInit,
                "xpu.spirv." + regName);
            spvGV->setAlignment(llvm::MaybeAlign(8));

            // ctor: __cajeta_xpu_register_module(regName, spvGV, len)
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

            // Per-kernel parameter metadata: which args are buffers vs scalars,
            // and the scalar byte sizes — so the runtime can bind buffers and
            // wrap scalars in single-element SSBOs at launch.
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
            b.CreateRetVoid();

            // Run at module-init time (LLJIT: jit->initialize; native: startup).
            llvm::appendToGlobalCtors(hostModule, ctor, /*priority=*/65535);
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
            // A ray-query kernel also gets a software variant "<name>$sw": the same
            // kernel lowered with the portable SoftwareRayQuery walk in plain
            // SPIR-V (AS bound as a storage buffer), selected at launch when the AS
            // was built as a software BVH on this backend (inc-4 brick #3).
            if (usesRayQuery(method))
                emitVariant(method, entryName + "$sw", /*software=*/true,
                            /*registerKparams=*/false);
        }
        return emitted;
    }

    // --- Graphics shader registration (cajeta-gfx §4) ----------------------

    namespace {
        // Map a graphics method's stage annotation onto the SPIR-V ShaderStage.
        // Returns nullopt if the method carries no graphics stage (caller should
        // have filtered on isGraphicsShader first).
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

    // The rasterization twin of emitKernelRegistration: each @Vertex/@Fragment/…
    // method is lowered on its OWN per-stage SPIR-V TargetMachine (the stage
    // execution model rides the triple env + the entry's hlsl.shader attr), then
    // its module is embedded + registered under the entry name exactly as a
    // kernel is — so a graphics shader rides the normal build like a @Kernel. No
    // kernel-param metadata is emitted: graphics stages bind inputs as interface
    // variables, not the uniform kernelParams argv (graphics resource descriptors
    // are a later slice). Stages whose body uses an unsupported construct
    // (XPU-N01) are skipped, never fatal.
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

        // void __cajeta_xpu_register_module_be(name, image, len, backend) — the
        // same backend-tagged hook the kernel path registers modules through.
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

            // A FRESH per-stage TargetMachine per shader (as the kernel path
            // does): the SPIR-V backend accumulates codegen state on the TM, so
            // it must not be reused across emitSpirv calls.
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
                // Unsupported construct (XPU-N01) — leave this stage to nobody;
                // don't fail the whole compile.
                continue;
            }
            if (!sfn) continue;

            std::vector<uint8_t> spirv = emitSpirv(devMod, *tm);
            if (spirv.empty()) continue;  // codegen error (logged)

            // Embed the SPIR-V as a private host-module constant.
            llvm::Constant* dataInit = llvm::ConstantDataArray::get(
                ctx, llvm::ArrayRef<uint8_t>(spirv.data(), spirv.size()));
            auto* spvGV = new llvm::GlobalVariable(
                hostModule, dataInit->getType(), /*isConstant=*/true,
                llvm::GlobalValue::PrivateLinkage, dataInit,
                "xpu.spirv." + regName);
            spvGV->setAlignment(llvm::MaybeAlign(8));

            // ctor: __cajeta_xpu_register_module(regName, spvGV, len)
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

            // Run at module-init time (LLJIT: jit->initialize; native: startup).
            llvm::appendToGlobalCtors(hostModule, ctor, /*priority=*/65535);
            ++emitted;
        }
        return emitted;
    }

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
